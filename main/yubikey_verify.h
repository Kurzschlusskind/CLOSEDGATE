/**
 * @file yubikey_verify.h
 * @brief Yubico Cloud API OTP Verification Client
 * 
 * This module handles OTP validation against the Yubico Cloud API,
 * including modhex validation, nonce generation, and HTTPS communication.
 */

#ifndef YUBIKEY_VERIFY_H
#define YUBIKEY_VERIFY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** YubiKey OTP length (always 44 characters) */
#define YUBIKEY_OTP_LEN 44

/** Nonce length (32 hex characters) */
#define YUBIKEY_NONCE_LEN 32

/** Verification result status codes */
typedef enum {
    YUBIKEY_STATUS_OK = 0,           /**< OTP is valid */
    YUBIKEY_STATUS_BAD_OTP,          /**< OTP is invalid format */
    YUBIKEY_STATUS_REPLAYED_OTP,     /**< OTP has already been used */
    YUBIKEY_STATUS_BAD_SIGNATURE,    /**< HMAC signature mismatch */
    YUBIKEY_STATUS_MISSING_PARAMETER,/**< Required parameter missing */
    YUBIKEY_STATUS_NO_SUCH_CLIENT,   /**< Unknown client ID */
    YUBIKEY_STATUS_OPERATION_NOT_ALLOWED, /**< Operation not allowed */
    YUBIKEY_STATUS_BACKEND_ERROR,    /**< Yubico server error */
    YUBIKEY_STATUS_NOT_ENOUGH_ANSWERS, /**< Not enough sync responses */
    YUBIKEY_STATUS_REPLAYED_REQUEST, /**< Request already seen */
    YUBIKEY_STATUS_NETWORK_ERROR,    /**< Network/HTTP error */
    YUBIKEY_STATUS_PARSE_ERROR,      /**< Response parse error */
    YUBIKEY_STATUS_UNKNOWN           /**< Unknown status */
} yubikey_status_t;

/**
 * @brief Initialize the Yubico verification client
 * 
 * @param client_id Yubico API client ID
 * @return ESP_OK on success
 */
esp_err_t yubikey_verify_init(const char *client_id);

/**
 * @brief Verify a YubiKey OTP against the Yubico Cloud API
 * 
 * @param otp The 44-character OTP string from YubiKey
 * @return yubikey_status_t indicating verification result
 */
yubikey_status_t yubikey_verify_otp(const char *otp);

/**
 * @brief Check if a string is valid modhex encoding
 * 
 * Modhex alphabet: cbdefghijklnrtuv
 * 
 * @param str String to validate
 * @param len Expected length (44 for YubiKey OTP)
 * @return true if valid modhex
 */
bool yubikey_is_valid_modhex(const char *str, size_t len);

/**
 * @brief Get human-readable status string
 * 
 * @param status Status code
 * @return Status string
 */
const char *yubikey_status_to_string(yubikey_status_t status);

/**
 * @brief De-initialize verification client
 */
void yubikey_verify_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* YUBIKEY_VERIFY_H */
