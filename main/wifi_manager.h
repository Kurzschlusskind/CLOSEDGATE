/**
 * @file wifi_manager.h
 * @brief WiFi Manager with automatic reconnection
 * 
 * This module handles WiFi STA mode initialization, connection,
 * and automatic reconnection with exponential backoff.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** WiFi connection state */
typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED
} wifi_state_t;

/** Callback function type for WiFi state changes */
typedef void (*wifi_state_callback_t)(wifi_state_t state);

/**
 * @brief Initialize WiFi manager
 * 
 * Initializes WiFi in STA mode and sets up event handlers.
 * 
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Start WiFi connection
 * 
 * Begins connection attempt to configured SSID.
 * 
 * @param ssid WiFi network name
 * @param password WiFi password
 * @param callback Optional callback for state changes
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_start(const char *ssid, const char *password, 
                             wifi_state_callback_t callback);

/**
 * @brief Wait for WiFi connection
 * 
 * Blocks until connected or timeout.
 * 
 * @param timeout_ms Timeout in milliseconds (0 for infinite)
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);

/**
 * @brief Get current WiFi state
 * 
 * @return Current connection state
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * @brief Check if WiFi is connected
 * 
 * @return true if connected with valid IP
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Stop WiFi and disconnect
 */
void wifi_manager_stop(void);

/**
 * @brief De-initialize WiFi manager
 */
void wifi_manager_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
