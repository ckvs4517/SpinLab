#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SPINLAB_BLE_SHOT_VALID = 0,
    SPINLAB_BLE_SHOT_INVALID_SHORT = 1,
    SPINLAB_BLE_SHOT_NO_REVERSAL = 2,
    SPINLAB_BLE_SHOT_OVERFLOW = 3,
} spinlab_ble_shot_status_t;

typedef struct {
    spinlab_ble_shot_status_t status;
    bool both_edges;
    bool rewind_anomaly;
    bool alternation_error;
    uint8_t reversal_type;
    uint32_t transitions;
    uint64_t pull_active_duration_us;
    uint64_t reversal_gap_us;
    float reference_sp_low;
    float reference_sp_mid;
    float reference_sp_high;
    float pull_peak_rpm;
} spinlab_ble_shot_t;

esp_err_t spinlab_ble_init(void);
esp_err_t spinlab_ble_publish_shot(const spinlab_ble_shot_t *shot);
bool spinlab_ble_is_connected(void);
