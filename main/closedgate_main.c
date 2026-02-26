/**
 * @file closedgate_main.c
 * @brief CLOSEDGATE Main Application
 * 
 * ESP32 NFC Access Controller main application.
 * Coordinates NFC polling, OTP/UID authentication, and relay control.
 * Supports YubiKey OTP (via Yubico Cloud API) and MIFARE Classic cards
 * (via local UID whitelist). Both modes can be enabled/disabled via Kconfig.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "nfc_handler.h"
#include "relay_control.h"
#include "sdkconfig.h"

#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
#include "yubikey_verify.h"
#include "wifi_manager.h"
#endif

#ifdef CONFIG_CLOSEDGATE_MIFARE_ENABLED
#include "mifare_auth.h"
#endif

static const char *TAG = "CLOSEDGATE";

#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
/* OTP processing queue */
#define OTP_QUEUE_SIZE 4
static QueueHandle_t s_otp_queue = NULL;

/* OTP queue item structure */
typedef struct {
    char otp[NFC_OTP_MAX_LEN];
    size_t len;
} otp_item_t;
#endif

/* Forward declarations */
#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
static void otp_received_callback(const char *otp, size_t len);
static void otp_processing_task(void *pvParameters);
static void wifi_state_changed(wifi_state_t state);
#endif

#ifdef CONFIG_CLOSEDGATE_MIFARE_ENABLED
static void uid_received_callback(const uint8_t *uid, size_t uid_len);
#endif

static esp_err_t init_nvs(void);

/**
 * @brief Application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  CLOSEDGATE - NFC Access Controller");
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "Firmware version: 1.0.0");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());

#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
    ESP_LOGI(TAG, "Mode: YubiKey OTP enabled");
#endif
#ifdef CONFIG_CLOSEDGATE_MIFARE_ENABLED
    ESP_LOGI(TAG, "Mode: MIFARE Classic UID enabled");
#endif
#if !defined(CONFIG_CLOSEDGATE_YUBIKEY_ENABLED) && !defined(CONFIG_CLOSEDGATE_MIFARE_ENABLED)
    ESP_LOGE(TAG, "FATAL: Both YubiKey and MIFARE modes are disabled - no authentication possible!");
    esp_restart();
#endif

    /* Initialize NVS */
    esp_err_t ret = init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed, restarting...");
        esp_restart();
    }

    /* Create default event loop */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop creation failed: %s", esp_err_to_name(ret));
        esp_restart();
    }

#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
    /* Create OTP processing queue */
    s_otp_queue = xQueueCreate(OTP_QUEUE_SIZE, sizeof(otp_item_t));
    if (s_otp_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create OTP queue");
        esp_restart();
    }
#endif

    /* Initialize relay control */
    ESP_LOGI(TAG, "Initializing relay control...");
    ret = relay_control_init(CONFIG_CLOSEDGATE_RELAY_GPIO, 
                             CONFIG_CLOSEDGATE_RELAY_PULSE_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Relay control init failed: %s", esp_err_to_name(ret));
        esp_restart();
    }

#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
    /* Initialize WiFi manager */
    ESP_LOGI(TAG, "Initializing WiFi manager...");
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi manager init failed: %s", esp_err_to_name(ret));
        esp_restart();
    }

    /* Start WiFi connection */
    ESP_LOGI(TAG, "Connecting to WiFi: %s", CONFIG_CLOSEDGATE_WIFI_SSID);
    ret = wifi_manager_start(CONFIG_CLOSEDGATE_WIFI_SSID,
                             CONFIG_CLOSEDGATE_WIFI_PASSWORD,
                             wifi_state_changed);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(ret));
        esp_restart();
    }

    /* Wait for WiFi connection (30 second timeout) */
    ret = wifi_manager_wait_connected(30000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connection timeout, continuing anyway...");
    }

    /* Initialize Yubico verification client */
    ESP_LOGI(TAG, "Initializing Yubico verification client...");
    ret = yubikey_verify_init(CONFIG_CLOSEDGATE_YUBICO_CLIENT_ID);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Yubico verify init failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Continuing without OTP verification capability");
    }
#endif /* CONFIG_CLOSEDGATE_YUBIKEY_ENABLED */

#ifdef CONFIG_CLOSEDGATE_MIFARE_ENABLED
    /* Initialize MIFARE authentication */
    ESP_LOGI(TAG, "Initializing MIFARE authentication...");
    ret = mifare_auth_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MIFARE auth init failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Continuing without MIFARE authentication capability");
    } else {
        ESP_LOGI(TAG, "MIFARE whitelist: %d UID(s) loaded", mifare_auth_get_uid_count());
    }
#endif /* CONFIG_CLOSEDGATE_MIFARE_ENABLED */

    /* Initialize NFC handler */
    ESP_LOGI(TAG, "Initializing NFC handler...");
    ret = nfc_handler_init(CONFIG_CLOSEDGATE_NFC_SDA_GPIO,
                           CONFIG_CLOSEDGATE_NFC_SCL_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NFC handler init failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Check NFC module wiring (SDA=%d, SCL=%d)",
                 CONFIG_CLOSEDGATE_NFC_SDA_GPIO, CONFIG_CLOSEDGATE_NFC_SCL_GPIO);
        /* Continue running to allow WiFi-based diagnostics */
    }

#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
    /* Create OTP processing task */
    BaseType_t task_ret = xTaskCreate(
        otp_processing_task,
        "otp_process",
        4096,
        NULL,
        4,
        NULL
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTP processing task");
        esp_restart();
    }
#endif

    /* Start NFC polling */
    if (nfc_handler_is_running() == false && ret == ESP_OK) {
        ret = nfc_handler_start(
#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
            otp_received_callback,
#else
            NULL,
#endif
#ifdef CONFIG_CLOSEDGATE_MIFARE_ENABLED
            uid_received_callback
#else
            NULL
#endif
        );
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NFC handler start failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "NFC polling started");
        }
    }

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  CLOSEDGATE initialization complete");
#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
    ESP_LOGI(TAG, "  YubiKey OTP: active");
#endif
#ifdef CONFIG_CLOSEDGATE_MIFARE_ENABLED
    ESP_LOGI(TAG, "  MIFARE Classic: active");
#endif
    ESP_LOGI(TAG, "  Waiting for NFC tap...");
    ESP_LOGI(TAG, "============================================");

    /* Main loop - monitor system status */
    while (1) {
        /* Periodic status logging */
        ESP_LOGI(TAG, "Status: WiFi=%s, NFC=%s, Relay=%s",
#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
                 wifi_manager_is_connected() ? "Connected" : "Disconnected",
#else
                 "N/A",
#endif
                 nfc_handler_is_running() ? "Running" : "Stopped",
                 relay_control_is_active() ? "Active" : "Idle");
        
        vTaskDelay(pdMS_TO_TICKS(60000));  /* Log every 60 seconds */
    }
}

/**
 * @brief Callback when NFC OTP is received
 * 
 * Called from NFC polling task context, queues OTP for processing.
 */
#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
static void otp_received_callback(const char *otp, size_t len)
{
    if (otp == NULL || len == 0) {
        ESP_LOGW(TAG, "Received empty OTP");
        return;
    }

    ESP_LOGI(TAG, "OTP received from NFC (len=%d)", (int)len);

    otp_item_t item;
    if (len >= sizeof(item.otp)) {
        len = sizeof(item.otp) - 1;
    }
    memcpy(item.otp, otp, len);
    item.otp[len] = '\0';
    item.len = len;

    /* Queue OTP for processing */
    if (xQueueSend(s_otp_queue, &item, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "OTP queue full, discarding");
    }
}
#endif /* CONFIG_CLOSEDGATE_YUBIKEY_ENABLED */

/**
 * @brief Callback when a MIFARE Classic UID is received
 *
 * Called from NFC polling task context, checks UID against whitelist.
 */
#ifdef CONFIG_CLOSEDGATE_MIFARE_ENABLED
static void uid_received_callback(const uint8_t *uid, size_t uid_len)
{
    if (uid == NULL || uid_len == 0) {
        ESP_LOGW(TAG, "Received empty UID");
        return;
    }

    ESP_LOGI(TAG, "UID received from NFC (len=%d)", (int)uid_len);

    if (mifare_auth_check_uid(uid, uid_len)) {
        ESP_LOGI(TAG, "=== ACCESS GRANTED (MIFARE) ===");
        relay_control_trigger();
    } else {
        ESP_LOGW(TAG, "=== ACCESS DENIED: UID not in whitelist ===");
    }
}
#endif /* CONFIG_CLOSEDGATE_MIFARE_ENABLED */

/**
 * @brief Task for processing OTPs
 * 
 * Validates OTPs against Yubico API and triggers relay on success.
 */
#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
static void otp_processing_task(void *pvParameters)
{
    (void)pvParameters;
    otp_item_t item;

    ESP_LOGI(TAG, "OTP processing task started");

    while (1) {
        /* Wait for OTP from queue */
        if (xQueueReceive(s_otp_queue, &item, portMAX_DELAY) == pdPASS) {
            ESP_LOGI(TAG, "Processing OTP: %s", item.otp);

            /* Check WiFi connection */
            if (!wifi_manager_is_connected()) {
                ESP_LOGW(TAG, "WiFi not connected, cannot verify OTP");
                continue;
            }

            /* Verify OTP against Yubico API */
            yubikey_status_t status = yubikey_verify_otp(item.otp);

            switch (status) {
                case YUBIKEY_STATUS_OK:
                    ESP_LOGI(TAG, "=== ACCESS GRANTED ===");
                    relay_control_trigger();
                    break;

                case YUBIKEY_STATUS_REPLAYED_OTP:
                    ESP_LOGW(TAG, "=== ACCESS DENIED: Replayed OTP ===");
                    break;

                case YUBIKEY_STATUS_BAD_OTP:
                    ESP_LOGW(TAG, "=== ACCESS DENIED: Invalid OTP format ===");
                    break;

                case YUBIKEY_STATUS_NO_SUCH_CLIENT:
                    ESP_LOGE(TAG, "=== ERROR: Invalid Yubico Client ID ===");
                    break;

                case YUBIKEY_STATUS_NETWORK_ERROR:
                    ESP_LOGE(TAG, "=== ERROR: Network communication failed ===");
                    break;

                default:
                    ESP_LOGW(TAG, "=== ACCESS DENIED: %s ===", 
                             yubikey_status_to_string(status));
                    break;
            }
        }
    }
}
#endif /* CONFIG_CLOSEDGATE_YUBIKEY_ENABLED */

/**
 * @brief Initialize NVS flash
 */
static esp_err_t init_nvs(void)
{
    ESP_LOGI(TAG, "Initializing NVS...");
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "NVS initialized");
    }
    
    return ret;
}

/**
 * @brief WiFi state change callback
 */
#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
static void wifi_state_changed(wifi_state_t state)
{
    switch (state) {
        case WIFI_STATE_CONNECTED:
            ESP_LOGI(TAG, "WiFi: Connected");
            break;
        case WIFI_STATE_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi: Disconnected");
            break;
        case WIFI_STATE_CONNECTING:
            ESP_LOGI(TAG, "WiFi: Connecting...");
            break;
        case WIFI_STATE_FAILED:
            ESP_LOGE(TAG, "WiFi: Connection failed");
            break;
    }
}
#endif /* CONFIG_CLOSEDGATE_YUBIKEY_ENABLED */
