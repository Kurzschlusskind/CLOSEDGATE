/**
 * @file nfc_handler.h
 * @brief PN532 NFC Handler for YubiKey OTP reading
 * 
 * This module provides I2C communication with PN532 NFC reader,
 * NDEF message parsing, and OTP extraction from YubiKey 5 NFC.
 */

#ifndef NFC_HANDLER_H
#define NFC_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of OTP string (YubiKey OTP is 44 characters) */
#define NFC_OTP_MAX_LEN 64

/** PN532 I2C address */
#define PN532_I2C_ADDRESS 0x24

/** Callback function type for OTP received events */
typedef void (*nfc_otp_callback_t)(const char *otp, size_t len);

/**
 * @brief Initialize the NFC handler
 * 
 * Initializes I2C bus and configures PN532 for ISO14443A operation.
 * 
 * @param sda_gpio GPIO pin for I2C SDA
 * @param scl_gpio GPIO pin for I2C SCL
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_handler_init(int sda_gpio, int scl_gpio);

/**
 * @brief Start the NFC polling task
 * 
 * Creates a FreeRTOS task that continuously polls for NFC tags.
 * When a YubiKey OTP is detected, the callback is invoked.
 * 
 * @param callback Function to call when OTP is read
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_handler_start(nfc_otp_callback_t callback);

/**
 * @brief Stop the NFC polling task
 * 
 * @return ESP_OK on success
 */
esp_err_t nfc_handler_stop(void);

/**
 * @brief Check if NFC handler is running
 * 
 * @return true if polling task is active
 */
bool nfc_handler_is_running(void);

/**
 * @brief De-initialize NFC handler and free resources
 */
void nfc_handler_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* NFC_HANDLER_H */
