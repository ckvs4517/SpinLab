#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spinlab_ble.h"

/* Demo-machine RPM sensor baseline. Keep this switch for A/B diagnostics. */
#define SPINLAB_IR_SENSOR_GPIO GPIO_NUM_0
#define SPINLAB_CAPTURE_BOTH_EDGES 1

#if SPINLAB_CAPTURE_BOTH_EDGES
#define SPINLAB_SENSOR_INTERRUPT_TYPE GPIO_INTR_ANYEDGE
#define SPINLAB_SENSOR_EDGE_MODE_NAME "both_edges"
#define SPINLAB_EDGES_PER_REVOLUTION 2U
#else
#define SPINLAB_SENSOR_INTERRUPT_TYPE GPIO_INTR_NEGEDGE
#define SPINLAB_SENSOR_EDGE_MODE_NAME "falling_edge"
#define SPINLAB_EDGES_PER_REVOLUTION 1U
#endif

/* Raw capture settings. No interval is discarded by the ISR. */
#define SPINLAB_CAPTURE_CAPACITY 4096U
#define SPINLAB_CAPTURE_IDLE_TIMEOUT_US 2000000ULL

/* Diagnostic labels are calculated only after capture has stopped. */
#define SPINLAB_SHORT_INTERVAL_US 500ULL
#define SPINLAB_GAP_HISTORY_SIZE 5U
#define SPINLAB_GAP_MIN_HISTORY 3U
#define SPINLAB_STRONG_GAP_RATIO_THRESHOLD 4.0f
#define SPINLAB_SMOOTH_GAP_RATIO_THRESHOLD 2.5f
#define SPINLAB_MIN_PULL_EDGES 8U
#define SPINLAB_REWIND_ANOMALY_RATIO 10.0f
#define SPINLAB_REWIND_ANOMALY_MIN_TRAILING_EDGES 2U

/* Third-party, uncalibrated reference model. Do not label as official SP. */
#define SPINLAB_REFERENCE_SP_SLOPE 43.195f
#define SPINLAB_REFERENCE_SP_INTERCEPT 81.402f

/* Demo-machine charge-status output: LOW means the battery is charging. */
#define SPINLAB_CHARGE_DETECT_GPIO GPIO_NUM_10
#define SPINLAB_CHARGE_ACTIVE_LEVEL 0
#define SPINLAB_CHARGE_CHECK_INTERVAL_US 1000000ULL

/* Demo-machine load sensor: HIGH means a Beyblade is installed. */
#define SPINLAB_LOAD_SENSOR_GPIO GPIO_NUM_1
#define SPINLAB_LOAD_ACTIVE_LEVEL 1
#define SPINLAB_LOAD_DEBOUNCE_US 1000ULL

/* ESP32-C3 SuperMini onboard blue LED: active-low on GPIO8. */
#define SPINLAB_STATUS_LED_GPIO GPIO_NUM_8
#define SPINLAB_STATUS_LED_ACTIVE_LEVEL 0
#define SPINLAB_BLE_BLINK_PERIOD_US 1000000ULL
#define SPINLAB_BLE_BLINK_ON_US 500000ULL
#define SPINLAB_CHARGE_BLINK_PERIOD_US 1200000ULL
#define SPINLAB_CHARGE_BLINK_ON_US 120000ULL
#define SPINLAB_CHARGE_BLINK_SECOND_US 240000ULL

typedef enum {
    CAPTURE_ARMED = 0,
    CAPTURE_ACTIVE,
    CAPTURE_DUMP_PENDING,
} capture_state_t;

typedef enum {
    REVERSAL_NONE = 0,
    REVERSAL_STRONG_GAP,
    REVERSAL_SMOOTH_GAPS,
} reversal_type_t;

typedef struct {
    uint32_t pull_start_index;
    int32_t reversal_index;
    reversal_type_t reversal_type;
    uint64_t pull_baseline_us;
    bool rewind_anomaly;
} shot_analysis_t;

static const char *TAG = "spinlab";

static uint64_t s_edge_timestamps_us[SPINLAB_CAPTURE_CAPACITY];
static uint8_t s_edge_levels[SPINLAB_CAPTURE_CAPACITY];
static spinlab_ble_raw_edge_t s_ble_raw_edges[SPINLAB_CAPTURE_CAPACITY];
static volatile uint32_t s_capture_count;
static volatile uint64_t s_last_edge_us;
static volatile bool s_capture_overflow;
static volatile capture_state_t s_capture_state = CAPTURE_ARMED;
static portMUX_TYPE s_capture_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_charge_state_initialized;
static bool s_charging;
static uint64_t s_last_charge_check_us;

static volatile uint8_t s_load_raw_level;
static volatile uint64_t s_load_last_raw_change_us;
static volatile bool s_load_change_pending;
static uint8_t s_load_stable_level;
static uint64_t s_load_last_stable_change_us;
static bool s_load_state_initialized;
static portMUX_TYPE s_load_mux = portMUX_INITIALIZER_UNLOCKED;

static void publish_device_status(void)
{
    const spinlab_ble_status_t status = {
        .load_initialized = s_load_state_initialized,
        .load_installed = s_load_state_initialized &&
                          s_load_stable_level == SPINLAB_LOAD_ACTIVE_LEVEL,
        .charging = s_charging,
        .load_raw_level = s_load_raw_level,
        .load_stable_level = s_load_stable_level,
    };

    const esp_err_t err = spinlab_ble_publish_status(&status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BLE status publish failed: %s", esp_err_to_name(err));
    }
}

static void set_status_led(bool on)
{
    gpio_set_level(SPINLAB_STATUS_LED_GPIO,
                   on ? SPINLAB_STATUS_LED_ACTIVE_LEVEL
                      : !SPINLAB_STATUS_LED_ACTIVE_LEVEL);
}

static void update_status_led(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    bool led_on;

    if (s_charging) {
        const uint64_t phase = now_us % SPINLAB_CHARGE_BLINK_PERIOD_US;
        led_on = phase < SPINLAB_CHARGE_BLINK_ON_US ||
                 (phase >= SPINLAB_CHARGE_BLINK_SECOND_US &&
                  phase < SPINLAB_CHARGE_BLINK_SECOND_US +
                              SPINLAB_CHARGE_BLINK_ON_US);
    } else if (spinlab_ble_is_connected()) {
        led_on = true;
    } else {
        led_on = now_us % SPINLAB_BLE_BLINK_PERIOD_US <
                 SPINLAB_BLE_BLINK_ON_US;
    }

    set_status_led(led_on);
}

static void IRAM_ATTR ir_edge_isr(void *arg)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint8_t level = (uint8_t)gpio_get_level(SPINLAB_IR_SENSOR_GPIO);
    (void)arg;

    portENTER_CRITICAL_ISR(&s_capture_mux);

    if (s_capture_state == CAPTURE_ARMED) {
        s_capture_count = 0;
        s_capture_overflow = false;
        s_capture_state = CAPTURE_ACTIVE;
    }

    if (s_capture_state == CAPTURE_ACTIVE) {
        if (s_capture_count < SPINLAB_CAPTURE_CAPACITY) {
            s_edge_timestamps_us[s_capture_count] = now_us;
            s_edge_levels[s_capture_count] = level;
            s_capture_count++;
        } else {
            s_capture_overflow = true;
        }

        s_last_edge_us = now_us;
    }

    portEXIT_CRITICAL_ISR(&s_capture_mux);
}

static void IRAM_ATTR load_edge_isr(void *arg)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint8_t level = (uint8_t)gpio_get_level(SPINLAB_LOAD_SENSOR_GPIO);
    (void)arg;

    portENTER_CRITICAL_ISR(&s_load_mux);
    s_load_raw_level = level;
    s_load_last_raw_change_us = now_us;
    s_load_change_pending = true;
    portEXIT_CRITICAL_ISR(&s_load_mux);
}

static float period_to_rpm(uint64_t period_us)
{
    if (period_us == 0) {
        return 0.0f;
    }

    return 60000000.0f /
           ((float)period_us * (float)SPINLAB_EDGES_PER_REVOLUTION);
}

static float calculate_reference_sp(int32_t n_transition, uint64_t duration_us)
{
    if (n_transition <= 0 || duration_us == 0) {
        return 0.0f;
    }

    const float transition_rate_hz =
        (float)n_transition * 1000000.0f / (float)duration_us;
    return SPINLAB_REFERENCE_SP_SLOPE * transition_rate_hz +
           SPINLAB_REFERENCE_SP_INTERCEPT;
}

static const char *edge_type_name(uint8_t level)
{
    return level == 0U ? "falling" : "rising";
}

static uint64_t median_period(const uint64_t *values, uint32_t count)
{
    uint64_t sorted[SPINLAB_MIN_PULL_EDGES - 1U];

    for (uint32_t i = 0; i < count; i++) {
        sorted[i] = values[i];
    }

    for (uint32_t i = 1; i < count; i++) {
        const uint64_t candidate = sorted[i];
        uint32_t j = i;

        while (j > 0 && sorted[j - 1] > candidate) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = candidate;
    }

    return sorted[count / 2U];
}

static void history_add(uint64_t *history, uint32_t *count, uint64_t period_us)
{
    if (*count < SPINLAB_GAP_HISTORY_SIZE) {
        history[*count] = period_us;
        (*count)++;
        return;
    }

    for (uint32_t i = 1; i < SPINLAB_GAP_HISTORY_SIZE; i++) {
        history[i - 1] = history[i];
    }
    history[SPINLAB_GAP_HISTORY_SIZE - 1U] = period_us;
}

static uint32_t find_pull_start(uint32_t count)
{
    if (count < SPINLAB_MIN_PULL_EDGES) {
        return 0;
    }

    for (uint32_t start = 0;
         start + SPINLAB_MIN_PULL_EDGES <= count;
         start++) {
        uint64_t intervals[SPINLAB_MIN_PULL_EDGES - 1U];
        bool coherent = true;

        for (uint32_t offset = 0; offset < SPINLAB_MIN_PULL_EDGES - 1U; offset++) {
            const uint32_t index = start + offset + 1U;
            intervals[offset] =
                s_edge_timestamps_us[index] - s_edge_timestamps_us[index - 1U];

            if (intervals[offset] < SPINLAB_SHORT_INTERVAL_US) {
                coherent = false;
                break;
            }
        }

        if (!coherent) {
            continue;
        }

        const uint64_t median_us =
            median_period(intervals, SPINLAB_MIN_PULL_EDGES - 1U);

        for (uint32_t offset = 0; offset < SPINLAB_MIN_PULL_EDGES - 1U; offset++) {
            if (median_us == 0 ||
                (float)intervals[offset] / (float)median_us >=
                    SPINLAB_STRONG_GAP_RATIO_THRESHOLD) {
                coherent = false;
                break;
            }
        }

        if (coherent) {
            return start;
        }
    }

    return 0;
}

static shot_analysis_t analyze_shot(uint32_t count)
{
    shot_analysis_t result = {
        .pull_start_index = find_pull_start(count),
        .reversal_index = -1,
        .reversal_type = REVERSAL_NONE,
        .pull_baseline_us = 0,
        .rewind_anomaly = false,
    };
    uint64_t history[SPINLAB_GAP_HISTORY_SIZE] = {0};
    uint32_t history_count = 0;
    int32_t pending_smooth_index = -1;
    uint64_t pending_smooth_delta_us = 0;
    uint64_t pending_baseline_us = 0;

    for (uint32_t i = result.pull_start_index + 1U; i < count; i++) {
        const uint64_t delta_us =
            s_edge_timestamps_us[i] - s_edge_timestamps_us[i - 1U];

        if (delta_us < SPINLAB_SHORT_INTERVAL_US) {
            continue;
        }

        if (history_count < SPINLAB_GAP_MIN_HISTORY) {
            history_add(history, &history_count, delta_us);
            continue;
        }

        uint64_t baseline_us = median_period(history, history_count);
        const bool enough_pull_edges =
            i - result.pull_start_index >= SPINLAB_MIN_PULL_EDGES;

        if (!enough_pull_edges || baseline_us == 0) {
            history_add(history, &history_count, delta_us);
            pending_smooth_index = -1;
            continue;
        }

        if (pending_smooth_index >= 0) {
            const float second_ratio =
                (float)delta_us / (float)pending_baseline_us;

            if (second_ratio >= SPINLAB_SMOOTH_GAP_RATIO_THRESHOLD) {
                result.reversal_index = pending_smooth_index;
                result.reversal_type = REVERSAL_SMOOTH_GAPS;
                result.pull_baseline_us = pending_baseline_us;
                break;
            }

            history_add(history, &history_count, pending_smooth_delta_us);
            pending_smooth_index = -1;
            baseline_us = median_period(history, history_count);
        }

        const float ratio = (float)delta_us / (float)baseline_us;

        if (ratio >= SPINLAB_STRONG_GAP_RATIO_THRESHOLD) {
            result.reversal_index = (int32_t)i;
            result.reversal_type = REVERSAL_STRONG_GAP;
            result.pull_baseline_us = baseline_us;
            break;
        }

        if (ratio >= SPINLAB_SMOOTH_GAP_RATIO_THRESHOLD) {
            pending_smooth_index = (int32_t)i;
            pending_smooth_delta_us = delta_us;
            pending_baseline_us = baseline_us;
        } else {
            history_add(history, &history_count, delta_us);
        }
    }

    if (result.reversal_index >= 0 && result.pull_baseline_us > 0) {
        for (uint32_t i = (uint32_t)result.reversal_index + 1U;
             i + SPINLAB_REWIND_ANOMALY_MIN_TRAILING_EDGES < count;
             i++) {
            const uint64_t delta_us =
                s_edge_timestamps_us[i] - s_edge_timestamps_us[i - 1U];
            const float ratio =
                (float)delta_us / (float)result.pull_baseline_us;

            if (ratio >= SPINLAB_REWIND_ANOMALY_RATIO) {
                result.rewind_anomaly = true;
                break;
            }
        }
    }

    return result;
}

static const char *reversal_type_name(reversal_type_t type)
{
    switch (type) {
    case REVERSAL_STRONG_GAP:
        return "strong_gap";
    case REVERSAL_SMOOTH_GAPS:
        return "smooth_gaps";
    case REVERSAL_NONE:
    default:
        return "none";
    }
}

static void dump_raw_capture(uint32_t count, bool overflow)
{
    const shot_analysis_t shot = analyze_shot(count);
    uint64_t history[SPINLAB_GAP_HISTORY_SIZE] = {0};
    uint32_t history_count = 0;
    uint32_t valid_interval_count = 0;
    uint32_t short_interval_count = 0;
    uint32_t gap_candidate_count = 0;
    int32_t first_gap_candidate_index = -1;

    ESP_LOGI(TAG, "capture complete; dumping raw edges");
    printf("RAW,index,timestamp_us,delta_us,rpm,gap_ratio,level,edge_type,"
           "classification\n");

    for (uint32_t i = 0; i < count; i++) {
        const uint64_t timestamp_us = s_edge_timestamps_us[i];
        const uint64_t delta_us =
            (i == 0) ? 0 : timestamp_us - s_edge_timestamps_us[i - 1U];
        float gap_ratio = 0.0f;
        const char *classification = "valid";
        bool gap_candidate = false;

        if (i == 0) {
            classification = "first_edge";
        } else if (delta_us < SPINLAB_SHORT_INTERVAL_US) {
            classification = "short_interval";
            short_interval_count++;
        } else {
            valid_interval_count++;

            if (history_count >= SPINLAB_GAP_MIN_HISTORY) {
                const uint64_t median_us = median_period(history, history_count);

                if (median_us > 0) {
                    gap_ratio = (float)delta_us / (float)median_us;
                    gap_candidate =
                        gap_ratio >= SPINLAB_STRONG_GAP_RATIO_THRESHOLD;
                }
            }

            if (gap_candidate) {
                classification = "phase_break_candidate";
                gap_candidate_count++;
                if (first_gap_candidate_index < 0) {
                    first_gap_candidate_index = (int32_t)i;
                }
            } else {
                history_add(history, &history_count, delta_us);
            }
        }

        printf("RAW,%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%.2f,%.2f,%u,%s,%s\n",
               i,
               timestamp_us,
               delta_us,
               period_to_rpm(delta_us),
               gap_ratio,
               (unsigned)s_edge_levels[i],
               edge_type_name(s_edge_levels[i]),
               classification);
    }

    fflush(stdout);

    const bool enough_edges = count >= SPINLAB_MIN_PULL_EDGES;
    const bool has_reversal = shot.reversal_index >= 0;
    const bool shot_valid = enough_edges && has_reversal && !overflow;
    const uint32_t pull_end_index =
        has_reversal ? (uint32_t)shot.reversal_index - 1U : shot.pull_start_index;
    const uint32_t pull_edges =
        has_reversal ? pull_end_index - shot.pull_start_index + 1U : 0U;
    const uint64_t pull_duration_us =
        has_reversal
            ? s_edge_timestamps_us[pull_end_index] -
                  s_edge_timestamps_us[shot.pull_start_index]
            : 0;
    const uint64_t reversal_gap_us =
        has_reversal
            ? s_edge_timestamps_us[(uint32_t)shot.reversal_index] -
                  s_edge_timestamps_us[pull_end_index]
            : 0;
    const uint64_t pull_to_first_rewind_us =
        has_reversal
            ? s_edge_timestamps_us[(uint32_t)shot.reversal_index] -
                  s_edge_timestamps_us[shot.pull_start_index]
            : 0;
    const int32_t n_transition =
        SPINLAB_CAPTURE_BOTH_EDGES && has_reversal ? (int32_t)pull_edges : -1;
    const float transition_rate_hz =
        n_transition > 0 && pull_duration_us > 0
            ? (float)n_transition * 1000000.0f / (float)pull_duration_us
            : 0.0f;
    const uint64_t reference_sp_mid_duration_us =
        pull_duration_us + reversal_gap_us / 2U;
    const float reference_sp_low =
        calculate_reference_sp(n_transition, pull_to_first_rewind_us);
    const float reference_sp_mid =
        calculate_reference_sp(n_transition, reference_sp_mid_duration_us);
    const float reference_sp_high =
        calculate_reference_sp(n_transition, pull_duration_us);
    const float reference_sp_uncertainty =
        reference_sp_high > reference_sp_low
            ? reference_sp_high - reference_sp_low
            : 0.0f;
    float pull_peak_rpm = 0.0f;
    uint32_t pull_falling_edges = 0;
    uint32_t pull_rising_edges = 0;
    uint32_t alternation_errors = 0;

    if (has_reversal) {
        for (uint32_t i = shot.pull_start_index; i <= pull_end_index; i++) {
            if (s_edge_levels[i] == 0U) {
                pull_falling_edges++;
            } else {
                pull_rising_edges++;
            }

            if (i > shot.pull_start_index &&
                s_edge_levels[i] == s_edge_levels[i - 1U]) {
                alternation_errors++;
            }
        }

        for (uint32_t i = shot.pull_start_index + 1U; i <= pull_end_index; i++) {
            const uint64_t delta_us =
                s_edge_timestamps_us[i] - s_edge_timestamps_us[i - 1U];
            const float rpm = period_to_rpm(delta_us);
            if (delta_us >= SPINLAB_SHORT_INTERVAL_US && rpm > pull_peak_rpm) {
                pull_peak_rpm = rpm;
            }
        }
    }

    const char *status = overflow                 ? "overflow"
                         : !enough_edges          ? "invalid_short_capture"
                         : !has_reversal          ? "no_reversal"
                                                  : "valid";
    const spinlab_ble_shot_status_t ble_status =
        overflow          ? SPINLAB_BLE_SHOT_OVERFLOW
        : !enough_edges   ? SPINLAB_BLE_SHOT_INVALID_SHORT
        : !has_reversal   ? SPINLAB_BLE_SHOT_NO_REVERSAL
                          : SPINLAB_BLE_SHOT_VALID;

    printf("SHOT,status,capture_mode,pull_start_index,pull_end_index,pull_edges,"
           "n_transition,pull_active_duration_us,reversal_gap_us,"
           "pull_to_first_rewind_us,transition_rate_hz,pull_peak_rpm,"
           "reference_sp_low,reference_sp_mid,reference_sp_high,"
           "reference_sp_uncertainty,"
           "pull_falling_edges,pull_rising_edges,alternation_errors,"
           "reversal_index,reversal_type,rewind_anomaly\n");
    printf("SHOT,%s,%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId32
           ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.2f,%.2f,%.1f,%.1f,"
           "%.1f,%.1f,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId32
           ",%s,%s\n",
           status,
           SPINLAB_SENSOR_EDGE_MODE_NAME,
           shot.pull_start_index,
           pull_end_index,
           pull_edges,
           n_transition,
           pull_duration_us,
           reversal_gap_us,
           pull_to_first_rewind_us,
           transition_rate_hz,
           pull_peak_rpm,
           reference_sp_low,
           reference_sp_mid,
           reference_sp_high,
           reference_sp_uncertainty,
           pull_falling_edges,
           pull_rising_edges,
           alternation_errors,
           shot.reversal_index,
           reversal_type_name(shot.reversal_type),
           shot.rewind_anomaly ? "yes" : "no");

    printf("\n"
           "================ SPINLAB SHOT RESULT ================\n"
           "Status             : %s\n"
           "Capture mode       : %s\n",
           status,
           SPINLAB_SENSOR_EDGE_MODE_NAME);

    if (shot_valid && n_transition > 0) {
        printf("Reference SP       : %.0f  (experimental / uncalibrated)\n"
               "Estimated range    : %.0f - %.0f\n"
               "Range width        : %.0f\n"
               "Transitions (N)    : %" PRId32
               "  [falling=%" PRIu32 ", rising=%" PRIu32 "]\n"
               "Alternation errors : %" PRIu32 "\n"
               "Pull active time   : %.3f ms\n"
               "Release time range : %.3f - %.3f ms\n"
               "Transition rate    : %.2f Hz\n"
               "Pull peak RPM      : %.2f\n"
               "Reversal           : index %" PRId32 " (%s)\n"
               "Rewind anomaly     : %s\n",
               reference_sp_mid,
               reference_sp_low,
               reference_sp_high,
               reference_sp_uncertainty,
               n_transition,
               pull_falling_edges,
               pull_rising_edges,
               alternation_errors,
               (double)pull_duration_us / 1000.0,
               (double)pull_duration_us / 1000.0,
               (double)pull_to_first_rewind_us / 1000.0,
               transition_rate_hz,
               pull_peak_rpm,
               shot.reversal_index,
               reversal_type_name(shot.reversal_type),
               shot.rewind_anomaly ? "yes" : "no");
    } else {
        printf("Reference SP       : unavailable\n"
               "Reason             : %s\n",
               status);
    }

    printf("=====================================================\n\n");
    fflush(stdout);

    const spinlab_ble_shot_t ble_shot = {
        .status = ble_status,
        .both_edges = SPINLAB_CAPTURE_BOTH_EDGES != 0,
        .rewind_anomaly = shot.rewind_anomaly,
        .alternation_error = alternation_errors > 0U,
        .reversal_type = (uint8_t)shot.reversal_type,
        .transitions = n_transition > 0 ? (uint32_t)n_transition : 0U,
        .pull_active_duration_us = pull_duration_us,
        .reversal_gap_us = reversal_gap_us,
        .reference_sp_low = reference_sp_low,
        .reference_sp_mid = reference_sp_mid,
        .reference_sp_high = reference_sp_high,
        .pull_peak_rpm = pull_peak_rpm,
    };
    uint16_t ble_shot_id = 0;
    const esp_err_t ble_result = spinlab_ble_publish_shot(&ble_shot, &ble_shot_id);
    if (ble_result != ESP_OK) {
        ESP_LOGW(TAG, "BLE shot publish failed: %s",
                 esp_err_to_name(ble_result));
    }
    const uint16_t raw_count = count > UINT16_MAX ? UINT16_MAX : (uint16_t)count;
    for (uint16_t i = 0; i < raw_count; ++i) {
        s_ble_raw_edges[i].timestamp_us = s_edge_timestamps_us[i];
        s_ble_raw_edges[i].delta_us = i == 0 ? 0U : (uint32_t)(s_edge_timestamps_us[i] - s_edge_timestamps_us[i - 1U]);
        s_ble_raw_edges[i].flags = (uint8_t)(s_edge_levels[i] & 1U);
    }
    spinlab_ble_publish_raw_profile(ble_shot_id, s_ble_raw_edges, raw_count);

    const uint64_t duration_us =
        (count > 1U) ? s_edge_timestamps_us[count - 1U] - s_edge_timestamps_us[0] : 0;
    ESP_LOGI(TAG,
             "capture summary: edges=%" PRIu32 " duration_ms=%" PRIu64
             " valid_intervals=%" PRIu32 " short_intervals=%" PRIu32
             " phase_break_candidates=%" PRIu32 " first_phase_break_index=%" PRId32
             " overflow=%s",
             count,
             duration_us / 1000ULL,
             valid_interval_count,
             short_interval_count,
             gap_candidate_count,
             first_gap_candidate_index,
             overflow ? "yes" : "no");

    ESP_LOGI(TAG,
             "shot segmentation: status=%s pull=%" PRIu32 "..%" PRIu32
             " reversal=%" PRId32 " (%s) rewind_anomaly=%s",
             shot_valid ? "valid" : status,
             shot.pull_start_index,
             pull_end_index,
             shot.reversal_index,
             reversal_type_name(shot.reversal_type),
             shot.rewind_anomaly ? "yes" : "no");
}

static void process_capture(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint32_t count = 0;
    bool overflow = false;
    bool should_dump = false;

    portENTER_CRITICAL(&s_capture_mux);
    if (s_capture_state == CAPTURE_ACTIVE && s_capture_count > 0U &&
        (s_capture_overflow ||
         now_us - s_last_edge_us >= SPINLAB_CAPTURE_IDLE_TIMEOUT_US)) {
        s_capture_state = CAPTURE_DUMP_PENDING;
        count = s_capture_count;
        overflow = s_capture_overflow;
        should_dump = true;
    }
    portEXIT_CRITICAL(&s_capture_mux);

    if (!should_dump) {
        return;
    }

    dump_raw_capture(count, overflow);

    portENTER_CRITICAL(&s_capture_mux);
    s_capture_count = 0;
    s_capture_overflow = false;
    s_last_edge_us = 0;
    s_capture_state = CAPTURE_ARMED;
    portEXIT_CRITICAL(&s_capture_mux);

    ESP_LOGI(TAG, "raw capture armed for the next launcher action");
}

static void update_charge_status(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();

    if (s_charge_state_initialized &&
        now_us - s_last_charge_check_us < SPINLAB_CHARGE_CHECK_INTERVAL_US) {
        return;
    }

    s_last_charge_check_us = now_us;
    const bool charging = gpio_get_level(SPINLAB_CHARGE_DETECT_GPIO) ==
                          SPINLAB_CHARGE_ACTIVE_LEVEL;

    if (!s_charge_state_initialized || charging != s_charging) {
        s_charging = charging;
        s_charge_state_initialized = true;
        ESP_LOGI(TAG, "charge status: %s", charging ? "charging" : "not charging");
        publish_device_status();
    }
}

static void initialize_load_status(void)
{
    const uint8_t level = (uint8_t)gpio_get_level(SPINLAB_LOAD_SENSOR_GPIO);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();

    portENTER_CRITICAL(&s_load_mux);
    s_load_raw_level = level;
    s_load_last_raw_change_us = now_us;
    s_load_change_pending = false;
    portEXIT_CRITICAL(&s_load_mux);

    s_load_stable_level = level;
    s_load_last_stable_change_us = now_us;
    s_load_state_initialized = true;

    ESP_LOGI(TAG,
             "load sensor ready: GPIO %d, raw=%u stable=%u installed=%s",
             SPINLAB_LOAD_SENSOR_GPIO,
             level,
             level,
             level == SPINLAB_LOAD_ACTIVE_LEVEL ? "yes" : "no");
}

static void update_load_status(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint8_t raw_level;
    uint64_t raw_change_us;
    bool pending;

    portENTER_CRITICAL(&s_load_mux);
    raw_level = s_load_raw_level;
    raw_change_us = s_load_last_raw_change_us;
    pending = s_load_change_pending;
    portEXIT_CRITICAL(&s_load_mux);

    if (!pending || now_us - raw_change_us < SPINLAB_LOAD_DEBOUNCE_US) {
        return;
    }

    if ((uint8_t)gpio_get_level(SPINLAB_LOAD_SENSOR_GPIO) != raw_level) {
        return;
    }

    portENTER_CRITICAL(&s_load_mux);
    if (s_load_last_raw_change_us != raw_change_us ||
        s_load_raw_level != raw_level) {
        portEXIT_CRITICAL(&s_load_mux);
        return;
    }
    s_load_change_pending = false;
    portEXIT_CRITICAL(&s_load_mux);

    if (raw_level == s_load_stable_level) {
        return;
    }

    const uint8_t old_level = s_load_stable_level;
    s_load_stable_level = raw_level;
    s_load_last_stable_change_us = raw_change_us;

    ESP_LOGI(TAG,
             "load stable change: timestamp_us=%" PRIu64
             " old=%u new=%u installed=%s debounce_us=%u",
             s_load_last_stable_change_us,
             old_level,
             s_load_stable_level,
             s_load_stable_level == SPINLAB_LOAD_ACTIVE_LEVEL ? "yes" : "no",
             (unsigned)SPINLAB_LOAD_DEBOUNCE_US);
    publish_device_status();
}

void app_main(void)
{
    const gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << SPINLAB_IR_SENSOR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = SPINLAB_SENSOR_INTERRUPT_TYPE,
    };
    const gpio_config_t charge_config = {
        .pin_bit_mask = 1ULL << SPINLAB_CHARGE_DETECT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t load_config = {
        .pin_bit_mask = 1ULL << SPINLAB_LOAD_SENSOR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    const gpio_config_t led_config = {
        .pin_bit_mask = 1ULL << SPINLAB_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&input_config));
    ESP_ERROR_CHECK(gpio_config(&charge_config));
    ESP_ERROR_CHECK(gpio_config(&load_config));
    ESP_ERROR_CHECK(gpio_config(&led_config));
    set_status_led(false);
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(SPINLAB_IR_SENSOR_GPIO, ir_edge_isr, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(SPINLAB_LOAD_SENSOR_GPIO,
                                         load_edge_isr,
                                         NULL));
    initialize_load_status();
    ESP_ERROR_CHECK(spinlab_ble_init());
    publish_device_status();

    ESP_LOGI(TAG,
             "raw capture ready: GPIO %d, pull-up, mode=%s, edges_per_rev=%u, "
             "capacity=%u, idle_timeout_ms=%u",
             SPINLAB_IR_SENSOR_GPIO,
             SPINLAB_SENSOR_EDGE_MODE_NAME,
             SPINLAB_EDGES_PER_REVOLUTION,
             SPINLAB_CAPTURE_CAPACITY,
             (unsigned)(SPINLAB_CAPTURE_IDLE_TIMEOUT_US / 1000ULL));

    while (true) {
        update_load_status();
        process_capture();
        update_charge_status();
        update_status_led();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
