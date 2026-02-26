#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUZZER_PATTERN_CARD_DETECTED = 0,
    BUZZER_PATTERN_ACCESS_GRANTED,
    BUZZER_PATTERN_ACCESS_DENIED,
    BUZZER_PATTERN_ERROR,
    BUZZER_PATTERN_BOOT_OK,
    BUZZER_PATTERN_WIFI_CONNECTED,
    BUZZER_PATTERN_WIFI_DISCONNECTED,
    BUZZER_PATTERN_MAX
} buzzer_pattern_t;

esp_err_t buzzer_init(int gpio_num);
esp_err_t buzzer_play_pattern(buzzer_pattern_t pattern);
esp_err_t buzzer_play_tone(uint32_t frequency_hz, uint32_t duration_ms);
void buzzer_stop(void);
void buzzer_set_enabled(bool enabled);
bool buzzer_is_enabled(void);
void buzzer_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_H */
