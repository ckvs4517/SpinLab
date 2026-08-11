#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Change this after wiring the Demo machine. The first milestone only proves
 * the interrupt -> timestamp -> period -> RPM path; it deliberately does not
 * apply filtering, BLE transport, or persistence yet.
 */
#define SPINLAB_IR_SENSOR_GPIO GPIO_NUM_2
#define SPINLAB_PULSES_PER_REVOLUTION 1U
#define SPINLAB_MIN_PERIOD_US 500U
#define SPINLAB_MAX_PERIOD_US 2000000U

static const char *TAG = "spinlab";

static volatile uint64_t s_previous_edge_us;
static volatile uint32_t s_period_us;
static volatile uint32_t s_edge_count;
static volatile bool s_period_ready;

static void IRAM_ATTR ir_edge_isr(void *arg)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint64_t previous_us = s_previous_edge_us;
    const uint64_t elapsed_us = now_us - previous_us;

    s_previous_edge_us = now_us;
    s_edge_count++;

    if (previous_us != 0 && elapsed_us >= SPINLAB_MIN_PERIOD_US &&
        elapsed_us <= SPINLAB_MAX_PERIOD_US) {
        s_period_us = (uint32_t)elapsed_us;
        s_period_ready = true;
    }
}

static float period_to_rpm(uint32_t period_us)
{
    return 60000000.0f / ((float)period_us * SPINLAB_PULSES_PER_REVOLUTION);
}

void app_main(void)
{
    const gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << SPINLAB_IR_SENSOR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ESP_ERROR_CHECK(gpio_config(&input_config));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(SPINLAB_IR_SENSOR_GPIO, ir_edge_isr, NULL));

    ESP_LOGI(TAG, "IR diagnostic ready on GPIO %d; pulses/rev=%u",
             SPINLAB_IR_SENSOR_GPIO, SPINLAB_PULSES_PER_REVOLUTION);

    while (true) {
        if (s_period_ready) {
            const uint32_t period_us = s_period_us;
            s_period_ready = false;
            ESP_LOGI(TAG, "edge=%" PRIu32 " period=%" PRIu32 " us rpm=%.1f",
                     s_edge_count, period_us, period_to_rpm(period_us));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
