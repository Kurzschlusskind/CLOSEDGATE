/**
 * @file closedgate_main.c
 * @brief CLOSEDGATE Main Application
 * 
 * ESP32 YubiKey NFC Access Controller main application.
 * Coordinates NFC polling, OTP verification, and relay control.
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
#include "yubikey_verify.h"
#include "wifi_manager.h"
#include "relay_control.h"
#include "sdkconfig.h"

static const char *TAG = "CLOSEDGATE";

/* OTP processing queue */
#define OTP_QUEUE_SIZE 4
static QueueHandle_t s_otp_queue = NULL;

/* OTP queue item structure */
typedef struct {
    char otp[NFC_OTP_MAX_LEN];
    size_t len;
} otp_item_t;

/* Forward declarations */
static void otp_received_callback(const char *otp, size_t len);
static void otp_processing_task(void *pvParameters);
static esp_err_t init_nvs(void);
static void wifi_state_changed(wifi_state_t state);

/**
 * @brief Application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  CLOSEDGATE - YubiKey NFC Access Controller");
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "Firmware version: 1.0.0");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());

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

    /* Create OTP processing queue */
    s_otp_queue = xQueueCreate(OTP_QUEUE_SIZE, sizeof(otp_item_t));
    if (s_otp_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create OTP queue");
        esp_restart();
    }

    /* Initialize relay control */
    ESP_LOGI(TAG, "Initializing relay control...");
    ret = relay_control_init(CONFIG_CLOSEDGATE_RELAY_GPIO, 
                             CONFIG_CLOSEDGATE_RELAY_PULSE_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Relay control init failed: %s", esp_err_to_name(ret));
        esp_restart();
    }

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

    /* Start NFC polling */
    if (nfc_handler_is_running() == false && ret == ESP_OK) {
        ret = nfc_handler_start(otp_received_callback);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NFC handler start failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "NFC polling started");
        }
    }

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  CLOSEDGATE initialization complete");
    ESP_LOGI(TAG, "  Waiting for YubiKey NFC tap...");
    ESP_LOGI(TAG, "============================================");

    /* Main loop - monitor system status */
    while (1) {
        /* Periodic status logging */
        ESP_LOGI(TAG, "Status: WiFi=%s, NFC=%s, Relay=%s",
                 wifi_manager_is_connected() ? "Connected" : "Disconnected",
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

/**
 * @brief Task for processing OTPs
 * 
 * Validates OTPs against Yubico API and triggers relay on success.
 */
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
