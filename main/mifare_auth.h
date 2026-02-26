/**
 * @file mifare_auth.h
 * @brief MIFARE Classic UID-based Authentication
 *
 * This module provides a UID whitelist stored in NVS for offline
 * MIFARE Classic card authentication.
 */

#ifndef MIFARE_AUTH_H
#define MIFARE_AUTH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MIFARE authentication module and load UIDs from NVS
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mifare_auth_init(void);

/**
 * @brief Check if a UID is in the whitelist
 *
 * @param uid    Pointer to UID bytes
 * @param uid_len Length of UID in bytes (4 or 7)
 * @return true if UID is whitelisted, false otherwise
 */
bool mifare_auth_check_uid(const uint8_t *uid, size_t uid_len);

/**
 * @brief Add a UID to the whitelist and persist to NVS
 *
 * @param uid    Pointer to UID bytes
 * @param uid_len Length of UID in bytes (4 or 7)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if whitelist is full
 */
esp_err_t mifare_auth_add_uid(const uint8_t *uid, size_t uid_len);

/**
 * @brief Remove a UID from the whitelist and persist to NVS
 *
 * @param uid    Pointer to UID bytes
 * @param uid_len Length of UID in bytes (4 or 7)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if UID not in whitelist
 */
esp_err_t mifare_auth_remove_uid(const uint8_t *uid, size_t uid_len);

/**
 * @brief Get the number of UIDs currently in the whitelist
 *
 * @return Number of stored UIDs
 */
int mifare_auth_get_uid_count(void);

/**
 * @brief De-initialize MIFARE authentication module and free resources
 */
void mifare_auth_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MIFARE_AUTH_H */
