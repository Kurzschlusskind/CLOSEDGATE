/**
 * @file yubikey_verify.c
 * @brief Yubico Cloud API OTP Verification Implementation
 * 
 * Implements OTP validation against the Yubico Cloud API with
 * modhex validation, nonce generation, and TLS communication.
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "yubikey_verify.h"
#include "sdkconfig.h"

static const char *TAG = "CLOSEDGATE";

/* Yubico API URL */
#define YUBICO_API_URL "https://api.yubico.com/wsapi/2.0/verify"

/* Modhex alphabet */
static const char MODHEX_ALPHABET[] = "cbdefghijklnrtuv";

/* Module state */
static char s_client_id[32] = {0};
static bool s_initialized = false;

/* HTTP response buffer */
#define HTTP_RESPONSE_MAX_LEN 1024
static char s_http_response[HTTP_RESPONSE_MAX_LEN];
static int s_http_response_len = 0;

/* Forward declarations */
static void generate_nonce(char *nonce_out, size_t len);
static yubikey_status_t parse_yubico_response(const char *response);
static esp_err_t http_event_handler(esp_http_client_event_t *evt);

esp_err_t yubikey_verify_init(const char *client_id)
{
    if (client_id == NULL || strlen(client_id) == 0) {
        ESP_LOGE(TAG, "Invalid client ID");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_client_id, client_id, sizeof(s_client_id) - 1);
    s_client_id[sizeof(s_client_id) - 1] = '\0';
    
    s_initialized = true;
    ESP_LOGI(TAG, "Yubico verify client initialized (ID: %s)", s_client_id);
    
    return ESP_OK;
}

bool yubikey_is_valid_modhex(const char *str, size_t len)
{
    if (str == NULL || len == 0) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        bool found = false;
        
        for (size_t j = 0; j < sizeof(MODHEX_ALPHABET) - 1; j++) {
            if (c == MODHEX_ALPHABET[j]) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            ESP_LOGW(TAG, "Invalid modhex character '%c' at position %d", c, (int)i);
            return false;
        }
    }
    
    return true;
}

yubikey_status_t yubikey_verify_otp(const char *otp)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Yubico verify client not initialized");
        return YUBIKEY_STATUS_NETWORK_ERROR;
    }

    if (otp == NULL) {
        ESP_LOGE(TAG, "OTP is NULL");
        return YUBIKEY_STATUS_BAD_OTP;
    }

    size_t otp_len = strlen(otp);
    
    /* YubiKey OTP is 44 characters */
    if (otp_len != YUBIKEY_OTP_LEN) {
        ESP_LOGW(TAG, "Invalid OTP length: %d (expected %d)", (int)otp_len, YUBIKEY_OTP_LEN);
        return YUBIKEY_STATUS_BAD_OTP;
    }

    /* Validate modhex encoding */
    if (!yubikey_is_valid_modhex(otp, otp_len)) {
        ESP_LOGW(TAG, "OTP contains invalid modhex characters");
        return YUBIKEY_STATUS_BAD_OTP;
    }

    /* Generate nonce */
    char nonce[YUBIKEY_NONCE_LEN + 1];
    generate_nonce(nonce, YUBIKEY_NONCE_LEN);
    nonce[YUBIKEY_NONCE_LEN] = '\0';

    ESP_LOGI(TAG, "Verifying OTP against Yubico API");
    ESP_LOGD(TAG, "Nonce: %s", nonce);

    /* Build URL with query parameters */
    char url[256];
    snprintf(url, sizeof(url), "%s?id=%s&otp=%s&nonce=%s",
             YUBICO_API_URL, s_client_id, otp, nonce);

    /* Perform HTTP request with retries */
    int retry_count = CONFIG_CLOSEDGATE_HTTP_RETRY_COUNT;
    yubikey_status_t status = YUBIKEY_STATUS_NETWORK_ERROR;

    for (int attempt = 1; attempt <= retry_count; attempt++) {
        ESP_LOGI(TAG, "HTTP request attempt %d/%d", attempt, retry_count);
        
        s_http_response_len = 0;
        memset(s_http_response, 0, sizeof(s_http_response));

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .event_handler = http_event_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            continue;
        }

        esp_err_t err = esp_http_client_perform(client);
        
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP Status = %d, content_length = %"PRId64,
                     status_code, esp_http_client_get_content_length(client));

            if (status_code == 200) {
                status = parse_yubico_response(s_http_response);
                esp_http_client_cleanup(client);
                break;
            } else {
                ESP_LOGW(TAG, "HTTP error status: %d", status_code);
                status = YUBIKEY_STATUS_NETWORK_ERROR;
            }
        } else {
            ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
            status = YUBIKEY_STATUS_NETWORK_ERROR;
        }

        esp_http_client_cleanup(client);

        /* Wait before retry */
        if (attempt < retry_count) {
            vTaskDelay(pdMS_TO_TICKS(1000 * attempt));  /* Exponential backoff */
        }
    }

    ESP_LOGI(TAG, "Verification result: %s", yubikey_status_to_string(status));
    return status;
}

const char *yubikey_status_to_string(yubikey_status_t status)
{
    switch (status) {
        case YUBIKEY_STATUS_OK:                 return "OK";
        case YUBIKEY_STATUS_BAD_OTP:            return "BAD_OTP";
        case YUBIKEY_STATUS_REPLAYED_OTP:       return "REPLAYED_OTP";
        case YUBIKEY_STATUS_BAD_SIGNATURE:      return "BAD_SIGNATURE";
        case YUBIKEY_STATUS_MISSING_PARAMETER:  return "MISSING_PARAMETER";
        case YUBIKEY_STATUS_NO_SUCH_CLIENT:     return "NO_SUCH_CLIENT";
        case YUBIKEY_STATUS_OPERATION_NOT_ALLOWED: return "OPERATION_NOT_ALLOWED";
        case YUBIKEY_STATUS_BACKEND_ERROR:      return "BACKEND_ERROR";
        case YUBIKEY_STATUS_NOT_ENOUGH_ANSWERS: return "NOT_ENOUGH_ANSWERS";
        case YUBIKEY_STATUS_REPLAYED_REQUEST:   return "REPLAYED_REQUEST";
        case YUBIKEY_STATUS_NETWORK_ERROR:      return "NETWORK_ERROR";
        case YUBIKEY_STATUS_PARSE_ERROR:        return "PARSE_ERROR";
        default:                                return "UNKNOWN";
    }
}

void yubikey_verify_deinit(void)
{
    s_initialized = false;
    memset(s_client_id, 0, sizeof(s_client_id));
    ESP_LOGI(TAG, "Yubico verify client deinitialized");
}

/* ============ Private Functions ============ */

static void generate_nonce(char *nonce_out, size_t len)
{
    static const char hex_chars[] = "0123456789abcdef";
    
    for (size_t i = 0; i < len; i++) {
        uint32_t rand_val = esp_random();
        nonce_out[i] = hex_chars[rand_val % 16];
    }
}

static yubikey_status_t parse_yubico_response(const char *response)
{
    if (response == NULL || strlen(response) == 0) {
        ESP_LOGE(TAG, "Empty response");
        return YUBIKEY_STATUS_PARSE_ERROR;
    }

    ESP_LOGD(TAG, "Yubico response:\n%s", response);

    /* Find status= line */
    const char *status_line = strstr(response, "status=");
    if (status_line == NULL) {
        ESP_LOGE(TAG, "No status field in response");
        return YUBIKEY_STATUS_PARSE_ERROR;
    }

    /* Extract status value */
    status_line += 7;  /* Skip "status=" */
    
    char status_value[32];
    int i = 0;
    while (status_line[i] != '\0' && status_line[i] != '\r' && 
           status_line[i] != '\n' && i < (int)sizeof(status_value) - 1) {
        status_value[i] = status_line[i];
        i++;
    }
    status_value[i] = '\0';

    ESP_LOGI(TAG, "Yubico status: %s", status_value);

    /* Map status string to enum */
    if (strcmp(status_value, "OK") == 0) {
        return YUBIKEY_STATUS_OK;
    } else if (strcmp(status_value, "BAD_OTP") == 0) {
        return YUBIKEY_STATUS_BAD_OTP;
    } else if (strcmp(status_value, "REPLAYED_OTP") == 0) {
        return YUBIKEY_STATUS_REPLAYED_OTP;
    } else if (strcmp(status_value, "BAD_SIGNATURE") == 0) {
        return YUBIKEY_STATUS_BAD_SIGNATURE;
    } else if (strcmp(status_value, "MISSING_PARAMETER") == 0) {
        return YUBIKEY_STATUS_MISSING_PARAMETER;
    } else if (strcmp(status_value, "NO_SUCH_CLIENT") == 0) {
        return YUBIKEY_STATUS_NO_SUCH_CLIENT;
    } else if (strcmp(status_value, "OPERATION_NOT_ALLOWED") == 0) {
        return YUBIKEY_STATUS_OPERATION_NOT_ALLOWED;
    } else if (strcmp(status_value, "BACKEND_ERROR") == 0) {
        return YUBIKEY_STATUS_BACKEND_ERROR;
    } else if (strcmp(status_value, "NOT_ENOUGH_ANSWERS") == 0) {
        return YUBIKEY_STATUS_NOT_ENOUGH_ANSWERS;
    } else if (strcmp(status_value, "REPLAYED_REQUEST") == 0) {
        return YUBIKEY_STATUS_REPLAYED_REQUEST;
    }

    ESP_LOGW(TAG, "Unknown status: %s", status_value);
    return YUBIKEY_STATUS_UNKNOWN;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", 
                     evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (s_http_response_len + evt->data_len < HTTP_RESPONSE_MAX_LEN) {
                memcpy(s_http_response + s_http_response_len, evt->data, evt->data_len);
                s_http_response_len += evt->data_len;
                s_http_response[s_http_response_len] = '\0';
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}
