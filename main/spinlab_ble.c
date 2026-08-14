#include "spinlab_ble.h"

#include <assert.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define SPINLAB_BLE_DEVICE_NAME "SpinLab"
#define SPINLAB_BLE_PROTOCOL_VERSION 1U

#define SPINLAB_BLE_FLAG_BOTH_EDGES (1U << 0)
#define SPINLAB_BLE_FLAG_REWIND_ANOMALY (1U << 1)
#define SPINLAB_BLE_FLAG_ALTERNATION_ERROR (1U << 2)

/*
 * Service UUID:        8f4e1000-9c3a-4f2b-a7d1-6b5c2e91a001
 * Result characteristic: 8f4e1000-9c3a-4f2b-a7d1-6b5c2e91a002
 */
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x01, 0xa0, 0x91, 0x2e, 0x5c, 0x6b, 0xd1, 0xa7,
                     0x2b, 0x4f, 0x3a, 0x9c, 0x00, 0x10, 0x4e, 0x8f);
static const ble_uuid128_t s_result_uuid =
    BLE_UUID128_INIT(0x02, 0xa0, 0x91, 0x2e, 0x5c, 0x6b, 0xd1, 0xa7,
                     0x2b, 0x4f, 0x3a, 0x9c, 0x00, 0x10, 0x4e, 0x8f);

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t status;
    uint8_t flags;
    uint8_t reversal_type;
    uint16_t shot_id;
    uint16_t reference_sp_mid;
    uint16_t reference_sp_low;
    uint16_t reference_sp_high;
    uint16_t transitions;
    uint16_t pull_active_time_0_1_ms;
    uint16_t reversal_gap_0_1_ms;
    uint16_t pull_peak_rpm;
} spinlab_ble_result_packet_t;

_Static_assert(sizeof(spinlab_ble_result_packet_t) == 20,
               "BLE result packet must fit the default ATT payload");

static const char *TAG = "spinlab_ble";
static uint8_t s_own_addr_type;
static uint16_t s_result_value_handle;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_notify_enabled;
static uint16_t s_next_shot_id = 1;
static spinlab_ble_result_packet_t s_latest_packet = {
    .version = SPINLAB_BLE_PROTOCOL_VERSION,
    .status = SPINLAB_BLE_SHOT_INVALID_SHORT,
};
static portMUX_TYPE s_ble_mux = portMUX_INITIALIZER_UNLOCKED;

static void start_advertising(void);

static uint16_t clamp_round_float(float value)
{
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 65535.0f) {
        return UINT16_MAX;
    }
    return (uint16_t)(value + 0.5f);
}

static uint16_t clamp_u32(uint32_t value)
{
    return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

static uint16_t duration_to_0_1_ms(uint64_t duration_us)
{
    const uint64_t units = (duration_us + 50U) / 100U;
    return units > UINT16_MAX ? UINT16_MAX : (uint16_t)units;
}

static int result_access(uint16_t conn_handle,
                         uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt,
                         void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    spinlab_ble_result_packet_t packet;
    portENTER_CRITICAL(&s_ble_mux);
    packet = s_latest_packet;
    portEXIT_CRITICAL(&s_ble_mux);

    return os_mbuf_append(ctxt->om, &packet, sizeof(packet)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &s_result_uuid.u,
                    .access_cb = result_access,
                    .val_handle = &s_result_value_handle,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                },
                {0},
            },
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            portENTER_CRITICAL(&s_ble_mux);
            s_connection_handle = event->connect.conn_handle;
            s_notify_enabled = false;
            portEXIT_CRITICAL(&s_ble_mux);
            ESP_LOGI(TAG, "app connected; handle=%u",
                     (unsigned)event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "connection failed; status=%d",
                     event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        portENTER_CRITICAL(&s_ble_mux);
        s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        portEXIT_CRITICAL(&s_ble_mux);
        ESP_LOGI(TAG, "app disconnected; reason=%d",
                 event->disconnect.reason);
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_result_value_handle) {
            portENTER_CRITICAL(&s_ble_mux);
            s_notify_enabled = event->subscribe.cur_notify != 0;
            portEXIT_CRITICAL(&s_ble_mux);
            ESP_LOGI(TAG, "result notifications: %s",
                     event->subscribe.cur_notify ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %u", (unsigned)event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_gap_adv_params params = {0};
    const char *name = ble_svc_gap_device_name();

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set advertising data; rc=%d", rc);
        return;
    }

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start advertising; rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "advertising as %s", SPINLAB_BLE_DEVICE_NAME);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address; rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to select BLE address; rc=%d", rc);
        return;
    }

    start_advertising();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t spinlab_ble_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to register GATT service; rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(SPINLAB_BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device name; rc=%d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE initialized; waiting for host sync");
    return ESP_OK;
}

esp_err_t spinlab_ble_publish_shot(const spinlab_ble_shot_t *shot)
{
    if (shot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    spinlab_ble_result_packet_t packet = {
        .version = SPINLAB_BLE_PROTOCOL_VERSION,
        .status = (uint8_t)shot->status,
        .flags = (shot->both_edges ? SPINLAB_BLE_FLAG_BOTH_EDGES : 0U) |
                 (shot->rewind_anomaly
                      ? SPINLAB_BLE_FLAG_REWIND_ANOMALY
                      : 0U) |
                 (shot->alternation_error
                      ? SPINLAB_BLE_FLAG_ALTERNATION_ERROR
                      : 0U),
        .reversal_type = shot->reversal_type,
        .reference_sp_mid = clamp_round_float(shot->reference_sp_mid),
        .reference_sp_low = clamp_round_float(shot->reference_sp_low),
        .reference_sp_high = clamp_round_float(shot->reference_sp_high),
        .transitions = clamp_u32(shot->transitions),
        .pull_active_time_0_1_ms =
            duration_to_0_1_ms(shot->pull_active_duration_us),
        .reversal_gap_0_1_ms =
            duration_to_0_1_ms(shot->reversal_gap_us),
        .pull_peak_rpm = clamp_round_float(shot->pull_peak_rpm),
    };
    uint16_t conn_handle;
    bool notify_enabled;

    portENTER_CRITICAL(&s_ble_mux);
    packet.shot_id = s_next_shot_id++;
    s_latest_packet = packet;
    conn_handle = s_connection_handle;
    notify_enabled = s_notify_enabled;
    portEXIT_CRITICAL(&s_ble_mux);

    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || !notify_enabled) {
        ESP_LOGI(TAG, "shot %u stored; no subscribed app",
                 (unsigned)packet.shot_id);
        return ESP_OK;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(&packet, sizeof(packet));
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int rc =
        ble_gatts_notify_custom(conn_handle, s_result_value_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "shot notification failed; rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "shot %u sent to app", (unsigned)packet.shot_id);
    return ESP_OK;
}

bool spinlab_ble_is_connected(void)
{
    bool connected;

    portENTER_CRITICAL(&s_ble_mux);
    connected = s_connection_handle != BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&s_ble_mux);

    return connected;
}
