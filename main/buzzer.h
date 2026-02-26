#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Predefined buzzer patterns */
typedef enum {
    BUZZER_PATTERN_CARD_DETECTED = 0,   /**< Card/tag detected: 1x short beep */
    BUZZER_PATTERN_ACCESS_GRANTED,       /**< Access granted: 2x ascending */
    BUZZER_PATTERN_ACCESS_DENIED,        /**< Access denied: 3x descending */
    BUZZER_PATTERN_ERROR,                /**< Error: fast beeping */
    BUZZER_PATTERN_BOOT_OK,              /**< System started: ascending melody */
    BUZZER_PATTERN_WIFI_CONNECTED,       /**< WiFi connected: 2x same tone */
    BUZZER_PATTERN_WIFI_DISCONNECTED,    /**< WiFi disconnected: 1x long low tone */
    BUZZER_PATTERN_MAX
} buzzer_pattern_t;

/**
 * @brief Initialize buzzer module
 *
 * Configures LEDC PWM for passive piezo buzzer (3-pin).
 *
 * @param gpio_num GPIO pin connected to buzzer signal pin
 * @return ESP_OK on success
 */
esp_err_t buzzer_init(int gpio_num);

/**
 * @brief Play a predefined buzzer pattern
 *
 * Non-blocking: creates a FreeRTOS task to play the pattern.
 *
 * @param pattern The pattern to play
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if buzzer disabled
 */
esp_err_t buzzer_play_pattern(buzzer_pattern_t pattern);

/**
 * @brief Play a single tone
 *
 * @param frequency_hz Tone frequency in Hz (100-10000)
 * @param duration_ms Tone duration in milliseconds
 * @return ESP_OK on success
 */
esp_err_t buzzer_play_tone(uint32_t frequency_hz, uint32_t duration_ms);

/**
 * @brief Stop any currently playing buzzer output
 */
void buzzer_stop(void);

/**
 * @brief Enable or disable buzzer globally
 *
 * @param enabled true to enable, false to mute
 */
void buzzer_set_enabled(bool enabled);

/**
 * @brief Check if buzzer is enabled
 *
 * @return true if enabled
 */
bool buzzer_is_enabled(void);

/**
 * @brief De-initialize buzzer
 */
void buzzer_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_H */
