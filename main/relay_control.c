/**
 * @file relay_control.c
 * @brief Relay Control Implementation
 * 
 * Implements GPIO-based relay control with non-blocking timer-based pulses.
 */

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "relay_control.h"

static const char *TAG = "CLOSEDGATE";

/* Module state */
static int s_gpio_num = -1;
static uint32_t s_default_pulse_ms = 3000;
static bool s_initialized = false;
static bool s_relay_active = false;
static TimerHandle_t s_pulse_timer = NULL;

/* Forward declarations */
static void pulse_timer_callback(TimerHandle_t xTimer);

esp_err_t relay_control_init(int gpio_num, uint32_t pulse_duration_ms)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Relay control already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing relay control (GPIO=%d, pulse=%"PRIu32"ms)", 
             gpio_num, pulse_duration_ms);

    s_gpio_num = gpio_num;
    s_default_pulse_ms = pulse_duration_ms;

    /* Configure GPIO as output */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set initial state to OFF */
    gpio_set_level(gpio_num, 0);
    s_relay_active = false;

    /* Create pulse timer */
    s_pulse_timer = xTimerCreate(
        "relay_pulse",
        pdMS_TO_TICKS(pulse_duration_ms),
        pdFALSE,  /* One-shot timer */
        NULL,
        pulse_timer_callback
    );

    if (s_pulse_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create pulse timer");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Relay control initialized");
    
    return ESP_OK;
}

esp_err_t relay_control_trigger(void)
{
    return relay_control_trigger_ms(s_default_pulse_ms);
}

esp_err_t relay_control_trigger_ms(uint32_t duration_ms)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Relay control not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_relay_active) {
        ESP_LOGW(TAG, "Relay already active, ignoring trigger");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Triggering relay for %"PRIu32" ms", duration_ms);

    /* Activate relay */
    gpio_set_level(s_gpio_num, 1);
    s_relay_active = true;

    /* Update timer period if different */
    if (xTimerChangePeriod(s_pulse_timer, pdMS_TO_TICKS(duration_ms), 
                          pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to change timer period");
        /* Fallback to blocking delay */
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        gpio_set_level(s_gpio_num, 0);
        s_relay_active = false;
        return ESP_OK;
    }

    /* Start timer to turn off relay */
    if (xTimerStart(s_pulse_timer, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start pulse timer");
        /* Fallback to immediate off */
        gpio_set_level(s_gpio_num, 0);
        s_relay_active = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool relay_control_is_active(void)
{
    return s_relay_active;
}

void relay_control_force_off(void)
{
    if (!s_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Forcing relay OFF");
    
    /* Stop timer if running */
    if (s_pulse_timer != NULL) {
        xTimerStop(s_pulse_timer, 0);
    }

    /* Turn off relay */
    gpio_set_level(s_gpio_num, 0);
    s_relay_active = false;
}

void relay_control_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    relay_control_force_off();

    if (s_pulse_timer != NULL) {
        xTimerDelete(s_pulse_timer, pdMS_TO_TICKS(100));
        s_pulse_timer = NULL;
    }

    s_initialized = false;
    s_gpio_num = -1;
    
    ESP_LOGI(TAG, "Relay control deinitialized");
}

/* ============ Private Functions ============ */

static void pulse_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    
    ESP_LOGI(TAG, "Relay pulse complete, turning OFF");
    gpio_set_level(s_gpio_num, 0);
    s_relay_active = false;
}
