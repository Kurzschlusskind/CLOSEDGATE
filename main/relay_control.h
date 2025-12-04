/**
 * @file relay_control.h
 * @brief Relay control module for door/gate triggering
 * 
 * This module handles GPIO-based relay control with
 * non-blocking timer-based pulse generation.
 */

#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize relay control
 * 
 * Configures GPIO as output and sets initial state to OFF.
 * 
 * @param gpio_num GPIO pin number for relay
 * @param pulse_duration_ms Default pulse duration in milliseconds
 * @return ESP_OK on success
 */
esp_err_t relay_control_init(int gpio_num, uint32_t pulse_duration_ms);

/**
 * @brief Trigger relay pulse
 * 
 * Activates relay for configured duration (non-blocking).
 * Uses timer to automatically deactivate.
 * 
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already active
 */
esp_err_t relay_control_trigger(void);

/**
 * @brief Trigger relay pulse with custom duration
 * 
 * @param duration_ms Pulse duration in milliseconds
 * @return ESP_OK on success
 */
esp_err_t relay_control_trigger_ms(uint32_t duration_ms);

/**
 * @brief Check if relay is currently active
 * 
 * @return true if relay is ON
 */
bool relay_control_is_active(void);

/**
 * @brief Force relay OFF immediately
 */
void relay_control_force_off(void);

/**
 * @brief De-initialize relay control
 */
void relay_control_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* RELAY_CONTROL_H */
