/**
 * @file buzzer.c
 * @brief Piezo Buzzer driver using ESP32 LEDC PWM
 *
 * Provides non-blocking acoustic feedback patterns via a passive piezo buzzer.
 * Uses LEDC low-speed mode (required for ESP32-C3).
 */

#include "buzzer.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "CLOSEDGATE";

#define BUZZER_LEDC_TIMER    LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BUZZER_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_RESOLUTION LEDC_TIMER_10_BIT
#define BUZZER_DUTY_MAX      1023

/* Tone step: frequency (Hz), duration (ms), pause after (ms) */
typedef struct {
    uint32_t freq_hz;
    uint32_t duration_ms;
    uint32_t pause_ms;
} buzzer_tone_t;

/* Pattern definitions */

/* Short single blip: card was read */
static const buzzer_tone_t s_pattern_card_detected[] = {
    { 1800, 55, 0 },
};

/* ACCESS GRANTED: rising do-mi-sol arpeggio — unmistakably positive */
static const buzzer_tone_t s_pattern_access_granted[] = {
    {  784, 120, 25 },   /* G5 */
    { 1047, 120, 25 },   /* C6 */
    { 1568, 320, 0  },   /* G6 — long final note */
};

/* ACCESS DENIED: two short stabs then a long low groan — unmistakably negative */
static const buzzer_tone_t s_pattern_access_denied[] = {
    { 1200, 90,  60 },
    { 1200, 90,  60 },
    {  300, 480, 0  },   /* deep low buzz */
};

/* SYSTEM ERROR: rapid high triple beep — "hardware problem" */
static const buzzer_tone_t s_pattern_error[] = {
    { 3500, 70, 45 },
    { 3500, 70, 45 },
    { 3500, 70, 0  },
};

/* BOOT OK: short ascending chime */
static const buzzer_tone_t s_pattern_boot_ok[] = {
    {  800, 80,  0 },
    { 1200, 80,  0 },
    { 1600, 80,  0 },
    { 2400, 180, 0 },
};

static const buzzer_tone_t s_pattern_wifi_connected[] = {
    { 1500, 100, 80 },
    { 1500, 100, 0 },
};

static const buzzer_tone_t s_pattern_wifi_disconnected[] = {
    { 500, 400, 0 },
};

typedef struct {
    const buzzer_tone_t *tones;
    size_t count;
} buzzer_pattern_def_t;

static const buzzer_pattern_def_t s_patterns[BUZZER_PATTERN_MAX] = {
    [BUZZER_PATTERN_CARD_DETECTED]    = { s_pattern_card_detected,    1 },
    [BUZZER_PATTERN_ACCESS_GRANTED]   = { s_pattern_access_granted,   2 },
    [BUZZER_PATTERN_ACCESS_DENIED]    = { s_pattern_access_denied,    3 },
    [BUZZER_PATTERN_ERROR]            = { s_pattern_error,            3 },
    [BUZZER_PATTERN_BOOT_OK]          = { s_pattern_boot_ok,          4 },
    [BUZZER_PATTERN_WIFI_CONNECTED]   = { s_pattern_wifi_connected,   2 },
    [BUZZER_PATTERN_WIFI_DISCONNECTED]= { s_pattern_wifi_disconnected,1 },
};

/* Module state */
static bool s_initialized = false;
static bool s_enabled = true;
static int  s_gpio_num = -1;
static TaskHandle_t s_pattern_task = NULL;
static volatile bool s_cancel_requested = false;

/* Task argument: pattern index */
typedef struct {
    buzzer_pattern_t pattern;
} buzzer_task_arg_t;

/**
 * @brief Set LEDC frequency and start buzzer output
 */
static void buzzer_hw_start(uint32_t freq_hz)
{
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq_hz);
    uint32_t duty = (uint32_t)(BUZZER_DUTY_MAX * CONFIG_CLOSEDGATE_BUZZER_VOLUME / 100);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

/**
 * @brief Stop buzzer output (duty = 0)
 */
static void buzzer_hw_stop(void)
{
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

/**
 * @brief FreeRTOS task that plays a pattern then deletes itself
 */
static void buzzer_pattern_task(void *pvParameters)
{
    buzzer_task_arg_t *arg = (buzzer_task_arg_t *)pvParameters;
    buzzer_pattern_t pattern = arg->pattern;
    free(arg);

    if (pattern >= BUZZER_PATTERN_MAX) {
        vTaskDelete(NULL);
        return;
    }

    const buzzer_pattern_def_t *def = &s_patterns[pattern];
    for (size_t i = 0; i < def->count && !s_cancel_requested; i++) {
        const buzzer_tone_t *tone = &def->tones[i];
        buzzer_hw_start(tone->freq_hz);
        vTaskDelay(pdMS_TO_TICKS(tone->duration_ms));
        buzzer_hw_stop();
        if (tone->pause_ms > 0 && !s_cancel_requested) {
            vTaskDelay(pdMS_TO_TICKS(tone->pause_ms));
        }
    }

    s_pattern_task = NULL;
    vTaskDelete(NULL);
}

/* --- Public API --- */

esp_err_t buzzer_init(int gpio_num)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Buzzer already initialized");
        return ESP_OK;
    }

    s_gpio_num = gpio_num;

    ledc_timer_config_t timer_cfg = {
        .speed_mode       = BUZZER_LEDC_MODE,
        .duty_resolution  = BUZZER_LEDC_RESOLUTION,
        .timer_num        = BUZZER_LEDC_TIMER,
        .freq_hz          = 1000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Buzzer LEDC timer config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t ch_cfg = {
        .speed_mode     = BUZZER_LEDC_MODE,
        .channel        = BUZZER_LEDC_CHANNEL,
        .timer_sel      = BUZZER_LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = gpio_num,
        .duty           = 0,
        .hpoint         = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Buzzer LEDC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Buzzer initialized on GPIO%d", gpio_num);
    return ESP_OK;
}

esp_err_t buzzer_play_pattern(buzzer_pattern_t pattern)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_enabled) {
        return ESP_OK;
    }
    if (pattern >= BUZZER_PATTERN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Stop any currently running pattern */
    if (s_pattern_task != NULL) {
        s_cancel_requested = true;
        /* Wait briefly for task to notice the cancellation flag */
        vTaskDelay(pdMS_TO_TICKS(20));
        s_pattern_task = NULL;
        buzzer_hw_stop();
    }

    s_cancel_requested = false;

    buzzer_task_arg_t *arg = malloc(sizeof(buzzer_task_arg_t));
    if (arg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    arg->pattern = pattern;

    BaseType_t ret = xTaskCreate(
        buzzer_pattern_task,
        "buzzer",
        2048,
        arg,
        2,
        &s_pattern_task
    );
    if (ret != pdPASS) {
        free(arg);
        s_pattern_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t buzzer_play_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_enabled) {
        return ESP_OK;
    }
    buzzer_hw_start(frequency_hz);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    buzzer_hw_stop();
    return ESP_OK;
}

void buzzer_stop(void)
{
    if (!s_initialized) {
        return;
    }
    if (s_pattern_task != NULL) {
        s_cancel_requested = true;
        s_pattern_task = NULL;
    }
    buzzer_hw_stop();
}

void buzzer_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        buzzer_stop();
    }
}

bool buzzer_is_enabled(void)
{
    return s_enabled;
}

void buzzer_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    buzzer_stop();
    ledc_stop(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    s_initialized = false;
    s_gpio_num = -1;
}
