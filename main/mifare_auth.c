/**
 * @file mifare_auth.c
 * @brief MIFARE Classic UID-based Authentication Implementation
 *
 * Implements a UID whitelist stored in NVS for offline MIFARE Classic
 * card authentication.
 */

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mifare_auth.h"
#include "sdkconfig.h"

static const char *TAG = "CLOSEDGATE";

/* NVS namespace and key for storing the UID list */
#define MIFARE_NVS_NAMESPACE    "mifare_auth"
#define MIFARE_NVS_KEY_COUNT    "uid_count"
#define MIFARE_NVS_KEY_PREFIX   "uid_"

/* Maximum UID length in bytes (7-byte UID = 14 hex chars + NUL) */
#define MIFARE_UID_MAX_BYTES    7
#define MIFARE_UID_HEX_LEN      (MIFARE_UID_MAX_BYTES * 2 + 1)

/* In-memory whitelist */
static char s_uid_list[CONFIG_CLOSEDGATE_MIFARE_MAX_UIDS][MIFARE_UID_HEX_LEN];
static int  s_uid_count = 0;
static bool s_initialized = false;

/* ============ Private helpers ============ */

/**
 * @brief Convert raw UID bytes to uppercase hex string (no separator).
 */
static void uid_to_hex(const uint8_t *uid, size_t uid_len, char *hex_out)
{
    for (size_t i = 0; i < uid_len; i++) {
        sprintf(&hex_out[i * 2], "%02X", uid[i]);
    }
    hex_out[uid_len * 2] = '\0';
}

/**
 * @brief Persist the current in-memory whitelist to NVS.
 */
static esp_err_t save_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(MIFARE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MIFARE: nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Store count */
    ret = nvs_set_i32(handle, MIFARE_NVS_KEY_COUNT, s_uid_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MIFARE: nvs_set_i32 failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    /* Store each UID */
    char key[16];
    for (int i = 0; i < s_uid_count; i++) {
        snprintf(key, sizeof(key), MIFARE_NVS_KEY_PREFIX "%d", i);
        ret = nvs_set_str(handle, key, s_uid_list[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MIFARE: nvs_set_str[%d] failed: %s", i, esp_err_to_name(ret));
            nvs_close(handle);
            return ret;
        }
    }

    ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

/**
 * @brief Load default UIDs from Kconfig string (comma-separated hex).
 */
static void load_default_uids(void)
{
    const char *defaults = CONFIG_CLOSEDGATE_MIFARE_DEFAULT_UIDS;
    if (defaults == NULL || defaults[0] == '\0') {
        return;
    }

    char buf[256];
    strncpy(buf, defaults, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);
    while (token != NULL) {
        /* Trim leading/trailing spaces */
        while (*token == ' ') token++;
        size_t tlen = strlen(token);
        while (tlen > 0 && token[tlen - 1] == ' ') {
            token[--tlen] = '\0';
        }

        if (tlen == 8 || tlen == 14) {
            /* Valid 4-byte (8 hex chars) or 7-byte (14 hex chars) UID */
            if (s_uid_count < CONFIG_CLOSEDGATE_MIFARE_MAX_UIDS) {
                /* Convert to uppercase for consistent comparison */
                for (size_t i = 0; i < tlen; i++) {
                    token[i] = (char)toupper((unsigned char)token[i]);
                }
                strncpy(s_uid_list[s_uid_count], token, MIFARE_UID_HEX_LEN - 1);
                s_uid_list[s_uid_count][MIFARE_UID_HEX_LEN - 1] = '\0';
                s_uid_count++;
                ESP_LOGI(TAG, "MIFARE: default UID loaded: %s", token);
            }
        } else {
            ESP_LOGW(TAG, "MIFARE: ignoring invalid default UID '%s' (len=%d)", token, (int)tlen);
        }

        token = strtok_r(NULL, ",", &saveptr);
    }
}

/* ============ Public API ============ */

esp_err_t mifare_auth_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "MIFARE: auth module already initialized");
        return ESP_OK;
    }

    s_uid_count = 0;
    memset(s_uid_list, 0, sizeof(s_uid_list));

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(MIFARE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* No data stored yet – load defaults and persist */
        ESP_LOGI(TAG, "MIFARE: no NVS data found, loading defaults");
        load_default_uids();
        s_initialized = true;
        if (s_uid_count > 0) {
            save_to_nvs();
        }
        ESP_LOGI(TAG, "MIFARE: auth initialized, %d UID(s) in whitelist", s_uid_count);
        return ESP_OK;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MIFARE: nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Read stored count */
    int32_t stored_count = 0;
    ret = nvs_get_i32(handle, MIFARE_NVS_KEY_COUNT, &stored_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MIFARE: uid_count key not found, starting empty");
        nvs_close(handle);
        s_initialized = true;
        return ESP_OK;
    }

    if (stored_count < 0) {
        stored_count = 0;
    }
    if (stored_count > CONFIG_CLOSEDGATE_MIFARE_MAX_UIDS) {
        stored_count = CONFIG_CLOSEDGATE_MIFARE_MAX_UIDS;
    }

    /* Read each UID */
    char key[16];
    for (int32_t i = 0; i < stored_count; i++) {
        snprintf(key, sizeof(key), MIFARE_NVS_KEY_PREFIX "%d", (int)i);
        size_t str_len = MIFARE_UID_HEX_LEN;
        ret = nvs_get_str(handle, key, s_uid_list[s_uid_count], &str_len);
        if (ret == ESP_OK) {
            s_uid_count++;
            ESP_LOGD(TAG, "MIFARE: loaded UID[%d]: %s", s_uid_count - 1,
                     s_uid_list[s_uid_count - 1]);
        } else {
            ESP_LOGW(TAG, "MIFARE: failed to read UID[%d]: %s", (int)i, esp_err_to_name(ret));
        }
    }

    nvs_close(handle);
    s_initialized = true;
    ESP_LOGI(TAG, "MIFARE: auth initialized, %d UID(s) in whitelist", s_uid_count);
    return ESP_OK;
}

bool mifare_auth_check_uid(const uint8_t *uid, size_t uid_len)
{
    if (!s_initialized || uid == NULL || uid_len == 0) {
        return false;
    }

    char hex[MIFARE_UID_HEX_LEN];
    uid_to_hex(uid, uid_len, hex);

    for (int i = 0; i < s_uid_count; i++) {
        if (strcmp(s_uid_list[i], hex) == 0) {
            ESP_LOGI(TAG, "MIFARE: UID %s found in whitelist", hex);
            return true;
        }
    }

    ESP_LOGW(TAG, "MIFARE: UID %s not in whitelist", hex);
    return false;
}

esp_err_t mifare_auth_add_uid(const uint8_t *uid, size_t uid_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (uid == NULL || uid_len == 0 || uid_len > MIFARE_UID_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_uid_count >= CONFIG_CLOSEDGATE_MIFARE_MAX_UIDS) {
        ESP_LOGE(TAG, "MIFARE: whitelist full (%d UIDs)", s_uid_count);
        return ESP_ERR_NO_MEM;
    }

    char hex[MIFARE_UID_HEX_LEN];
    uid_to_hex(uid, uid_len, hex);

    /* Check for duplicate */
    for (int i = 0; i < s_uid_count; i++) {
        if (strcmp(s_uid_list[i], hex) == 0) {
            ESP_LOGW(TAG, "MIFARE: UID %s already in whitelist", hex);
            return ESP_OK;
        }
    }

    strncpy(s_uid_list[s_uid_count], hex, MIFARE_UID_HEX_LEN - 1);
    s_uid_list[s_uid_count][MIFARE_UID_HEX_LEN - 1] = '\0';
    s_uid_count++;

    ESP_LOGI(TAG, "MIFARE: UID %s added to whitelist (%d total)", hex, s_uid_count);
    return save_to_nvs();
}

esp_err_t mifare_auth_remove_uid(const uint8_t *uid, size_t uid_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (uid == NULL || uid_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char hex[MIFARE_UID_HEX_LEN];
    uid_to_hex(uid, uid_len, hex);

    for (int i = 0; i < s_uid_count; i++) {
        if (strcmp(s_uid_list[i], hex) == 0) {
            /* Shift remaining entries down */
            for (int j = i; j < s_uid_count - 1; j++) {
                strncpy(s_uid_list[j], s_uid_list[j + 1], MIFARE_UID_HEX_LEN);
            }
            memset(s_uid_list[s_uid_count - 1], 0, MIFARE_UID_HEX_LEN);
            s_uid_count--;

            ESP_LOGI(TAG, "MIFARE: UID %s removed from whitelist (%d remaining)", hex, s_uid_count);
            return save_to_nvs();
        }
    }

    ESP_LOGW(TAG, "MIFARE: UID %s not found for removal", hex);
    return ESP_ERR_NOT_FOUND;
}

int mifare_auth_get_uid_count(void)
{
    return s_uid_count;
}

void mifare_auth_deinit(void)
{
    if (s_initialized) {
        s_uid_count = 0;
        memset(s_uid_list, 0, sizeof(s_uid_list));
        s_initialized = false;
        ESP_LOGI(TAG, "MIFARE: auth deinitialized");
    }
}
