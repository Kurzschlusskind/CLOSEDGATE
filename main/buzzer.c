/**
 * @file buzzer.c
 * @brief Piezo Buzzer Module for CLOSEDGATE
 *
 * Controls a passive 3-pin piezo buzzer via ESP32 LEDC PWM.
 * Provides non-blocking pattern playback using FreeRTOS tasks.
 */

#include "buzzer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"

#define TAG "CLOSEDGATE"

/* LEDC configuration */
#define BUZZER_LEDC_TIMER      LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL    LEDC_CHANNEL_0
#define BUZZER_LEDC_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_RESOLUTION LEDC_TIMER_10_BIT  /* 10-bit: 0-1023 */

/* Tone task stack and priority */
#define BUZZER_TASK_STACK_SIZE 2048
#define BUZZER_TASK_PRIORITY   2

/* Task stop notification value */
#define BUZZER_STOP_NOTIFICATION 1U

/** A single tone element: frequency (0 = pause) and duration */
typedef struct {
    uint32_t frequency_hz;
    uint32_t duration_ms;
} buzzer_tone_t;

/* --- Pattern definitions -------------------------------------------------- */

static const buzzer_tone_t s_pattern_card_detected[] = {
    {2000, 80},
};

static const buzzer_tone_t s_pattern_access_granted[] = {
    {1000, 120},
    {0,    50},
    {2000, 120},
};

static const buzzer_tone_t s_pattern_access_denied[] = {
    {2000, 150},
    {0,    80},
    {1500, 150},
    {0,    80},
    {800,  300},
};

static const buzzer_tone_t s_pattern_error[] = {
    {3000, 50},
    {0,    50},
    {3000, 50},
    {0,    50},
    {3000, 50},
    {0,    50},
};

static const buzzer_tone_t s_pattern_boot_ok[] = {
    {800,  100},
    {1200, 100},
    {1600, 100},
    {2400, 200},
};

static const buzzer_tone_t s_pattern_wifi_connected[] = {
    {1500, 100},
    {0,    80},
    {1500, 100},
};

static const buzzer_tone_t s_pattern_wifi_disconnected[] = {
    {500, 400},
};

typedef struct {
    const buzzer_tone_t *tones;
    size_t               count;
} buzzer_pattern_def_t;

static const buzzer_pattern_def_t s_patterns[BUZZER_PATTERN_MAX] = {
    [BUZZER_PATTERN_CARD_DETECTED]    = {s_pattern_card_detected,     1},
    [BUZZER_PATTERN_ACCESS_GRANTED]   = {s_pattern_access_granted,    3},
    [BUZZER_PATTERN_ACCESS_DENIED]    = {s_pattern_access_denied,     5},
    [BUZZER_PATTERN_ERROR]            = {s_pattern_error,             6},
    [BUZZER_PATTERN_BOOT_OK]          = {s_pattern_boot_ok,           4},
    [BUZZER_PATTERN_WIFI_CONNECTED]   = {s_pattern_wifi_connected,    3},
    [BUZZER_PATTERN_WIFI_DISCONNECTED]= {s_pattern_wifi_disconnected, 1},
};

/* --- Module state --------------------------------------------------------- */

static bool s_initialized = false;
static bool s_enabled     = true;
static int  s_gpio_num    = -1;
static TaskHandle_t s_tone_task = NULL;

/* Static pattern arg shared with the tone task (protected by task lifecycle) */
static buzzer_pattern_def_t s_current_pattern;

/* -------------------------------------------------------------------------- */

/**
 * @brief Set LEDC output for a given frequency at the configured volume.
 *        Passing frequency_hz == 0 silences the output.
 */
static void buzzer_set_freq(uint32_t frequency_hz)
{
    if (frequency_hz == 0) {
        ledc_set_duty(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL, 0);
        ledc_update_duty(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL);
        return;
    }

    /* Reconfigure timer frequency */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BUZZER_LEDC_SPEED_MODE,
        .duty_resolution = BUZZER_LEDC_RESOLUTION,
        .timer_num       = BUZZER_LEDC_TIMER,
        .freq_hz         = frequency_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    /* Calculate duty cycle from configured volume (10-bit resolution) */
    uint32_t duty = ((1u << 10) - 1) * CONFIG_CLOSEDGATE_BUZZER_VOLUME / 100;

    ledc_set_duty(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL, duty);
    ledc_update_duty(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL);
}

/* -------------------------------------------------------------------------- */

/**
 * @brief FreeRTOS task that plays a sequence of tones.
 *
 * Reads from the global s_current_pattern. Exits immediately if a stop
 * notification is received between tones.
 */
static void tone_task(void *pvParameters)
{
    (void)pvParameters;

    for (size_t i = 0; i < s_current_pattern.count; i++) {
        buzzer_set_freq(s_current_pattern.tones[i].frequency_hz);

        /* Sleep for tone duration; wake early if a stop notification arrives */
        uint32_t notif_val = 0;
        xTaskNotifyWait(0, ULONG_MAX, &notif_val,
                        pdMS_TO_TICKS(s_current_pattern.tones[i].duration_ms));
        if (notif_val != 0) {
            break;
        }
    }

    /* Silence output when done or aborted */
    buzzer_set_freq(0);

    s_tone_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */

esp_err_t buzzer_init(int gpio_num)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Buzzer already initialized");
        return ESP_OK;
    }

    s_gpio_num = gpio_num;

    /* Configure LEDC timer */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BUZZER_LEDC_SPEED_MODE,
        .duty_resolution = BUZZER_LEDC_RESOLUTION,
        .timer_num       = BUZZER_LEDC_TIMER,
        .freq_hz         = 1000,  /* Initial frequency, overridden on first play */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Buzzer LEDC timer config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure LEDC channel */
    ledc_channel_config_t channel_cfg = {
        .gpio_num   = gpio_num,
        .speed_mode = BUZZER_LEDC_SPEED_MODE,
        .channel    = BUZZER_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BUZZER_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&channel_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Buzzer LEDC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Buzzer initialized on GPIO%d", gpio_num);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

esp_err_t buzzer_play_pattern(buzzer_pattern_t pattern)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "Buzzer not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pattern >= BUZZER_PATTERN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Signal any running tone task to stop gracefully */
    if (s_tone_task != NULL) {
        xTaskNotify(s_tone_task, BUZZER_STOP_NOTIFICATION, eSetValueWithOverwrite);
        /* Allow the task to process the notification and exit cleanly */
        for (int i = 0; i < 10 && s_tone_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        /* Force-delete only if the task has not exited within the timeout */
        if (s_tone_task != NULL) {
            vTaskDelete(s_tone_task);
            s_tone_task = NULL;
            buzzer_set_freq(0);
        }
    }

    /* Copy pattern definition into the static slot before creating the task */
    s_current_pattern.tones = s_patterns[pattern].tones;
    s_current_pattern.count = s_patterns[pattern].count;

    BaseType_t res = xTaskCreate(tone_task, "buzzer_tone",
                                 BUZZER_TASK_STACK_SIZE, NULL,
                                 BUZZER_TASK_PRIORITY, &s_tone_task);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Buzzer: failed to create tone task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

esp_err_t buzzer_play_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    buzzer_set_freq(frequency_hz);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    buzzer_set_freq(0);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

void buzzer_stop(void)
{
    if (s_tone_task != NULL) {
        xTaskNotify(s_tone_task, BUZZER_STOP_NOTIFICATION, eSetValueWithOverwrite);
        /* Allow graceful exit */
        for (int i = 0; i < 10 && s_tone_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (s_tone_task != NULL) {
            vTaskDelete(s_tone_task);
            s_tone_task = NULL;
        }
    }
    if (s_initialized) {
        buzzer_set_freq(0);
        ledc_timer_pause(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_TIMER);
    }
}

/* -------------------------------------------------------------------------- */

void buzzer_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        buzzer_stop();
    }
}

/* -------------------------------------------------------------------------- */

bool buzzer_is_enabled(void)
{
    return s_enabled;
}

/* -------------------------------------------------------------------------- */

void buzzer_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    buzzer_stop();
    ledc_stop(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL, 0);
    s_initialized = false;
    s_gpio_num    = -1;
    ESP_LOGI(TAG, "Buzzer de-initialized");
}
