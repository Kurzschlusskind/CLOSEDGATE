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
#include "esp_timer.h"
#include "driver/gpio.h"

#include "nfc_handler.h"
#include "relay_control.h"
#include "sdkconfig.h"

#ifdef CONFIG_CLOSEDGATE_BUZZER_ENABLED
#include "buzzer.h"
#endif

#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
#include "yubikey_verify.h"
#include "wifi_manager.h"
#endif

#ifdef CONFIG_CLOSEDGATE_MIFARE_ENABLED
#include "mifare_auth.h"
#endif

static const char *TAG = "CLOSEDGATE";

/* ============ Enroll Mode ============ */
#ifdef CONFIG_CLOSEDGATE_ENROLL_ENABLED
static volatile bool s_enroll_mode = false;
static volatile int64_t s_enroll_deadline = 0;

/**
 * @brief Task that polls the BOOT button and manages enroll mode.
 *
 * Polling avoids ISR issues with esp_timer. Checks button every 50ms.
 */
static void enroll_button_task(void *pvParameters)
{
    (void)pvParameters;
    bool last_level = true;  /* Pull-up = HIGH when not pressed */

    while (1) {
        bool level = gpio_get_level(CONFIG_CLOSEDGATE_ENROLL_GPIO);

        /* Detect falling edge (button just pressed) */
        if (last_level && !level) {
            if (!s_enroll_mode) {
                s_enroll_mode = true;
                s_enroll_deadline = esp_timer_get_time() +
                    (int64_t)CONFIG_CLOSEDGATE_ENROLL_TIMEOUT_S * 1000000LL;
                ESP_LOGW(TAG, ">>> ENROLL MODE ACTIVE for %ds - tap a card now! <<<",
                         CONFIG_CLOSEDGATE_ENROLL_TIMEOUT_S);
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED)
                /* Short beep to confirm enroll mode */
                buzzer_play_tone(1000, 150);
#endif
            }
        }
        last_level = level;

        /* Check timeout */
        if (s_enroll_mode && esp_timer_get_time() >= s_enroll_deadline) {
            s_enroll_mode = false;
            ESP_LOGI(TAG, ">>> ENROLL MODE TIMEOUT - back to normal <<<");
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED)
                buzzer_play_tone(400, 200);
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void enroll_init(void)
{
    /* Configure enroll button GPIO (input with pull-up, no interrupt) */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_CLOSEDGATE_ENROLL_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* Create polling task */
    xTaskCreate(enroll_button_task, "enroll_btn", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "Enroll button initialized on GPIO %d (timeout %ds)",
             CONFIG_CLOSEDGATE_ENROLL_GPIO, CONFIG_CLOSEDGATE_ENROLL_TIMEOUT_S);
}
#endif /* CONFIG_CLOSEDGATE_ENROLL_ENABLED */

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

#ifdef CONFIG_CLOSEDGATE_BUZZER_ENABLED
    ESP_LOGI(TAG, "Initializing buzzer...");
    ret = buzzer_init(CONFIG_CLOSEDGATE_BUZZER_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer init failed: %s", esp_err_to_name(ret));
    }
#endif

#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
    /* Only attempt WiFi if a real SSID is configured (not the default placeholder) */
    if (strcmp(CONFIG_CLOSEDGATE_WIFI_SSID, "YourWiFiSSID") != 0 &&
        strlen(CONFIG_CLOSEDGATE_WIFI_SSID) > 0) {

        ESP_LOGI(TAG, "Initializing WiFi manager...");
        ret = wifi_manager_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WiFi manager init failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Connecting to WiFi: %s", CONFIG_CLOSEDGATE_WIFI_SSID);
            ret = wifi_manager_start(CONFIG_CLOSEDGATE_WIFI_SSID,
                                     CONFIG_CLOSEDGATE_WIFI_PASSWORD,
                                     wifi_state_changed);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(ret));
            } else {
                /* Short timeout - don't block forever */
                ret = wifi_manager_wait_connected(5000);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "WiFi not connected yet, continuing (will retry in background)");
                }
            }
        }

        /* Initialize Yubico verification client */
        ESP_LOGI(TAG, "Initializing Yubico verification client...");
        ret = yubikey_verify_init(CONFIG_CLOSEDGATE_YUBICO_CLIENT_ID);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Yubico verify init failed: %s", esp_err_to_name(ret));
            ESP_LOGW(TAG, "Continuing without OTP verification capability");
        }
    } else {
        ESP_LOGW(TAG, "WiFi SSID not configured - skipping WiFi & YubiKey OTP");
        ESP_LOGW(TAG, "Use 'idf.py menuconfig' to set WiFi credentials");
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

#ifdef CONFIG_CLOSEDGATE_ENROLL_ENABLED
    /* Initialize enroll button */
    enroll_init();
#endif

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
#ifdef CONFIG_CLOSEDGATE_ENROLL_ENABLED
    ESP_LOGI(TAG, "  Enroll: press BOOT button + tap card");
#endif
    ESP_LOGI(TAG, "  Waiting for NFC tap...");
    ESP_LOGI(TAG, "============================================");

#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_BOOT_SOUND)
    buzzer_play_pattern(BUZZER_PATTERN_BOOT_OK);
#endif

    /* Main loop - monitor system status */
    while (1) {
        /* Periodic status logging */
        ESP_LOGI(TAG, "Status: WiFi=%s, NFC=%s, Relay=%s, Enroll=%s",
#ifdef CONFIG_CLOSEDGATE_YUBIKEY_ENABLED
                 wifi_manager_is_connected() ? "Connected" : "Disconnected",
#else
                 "N/A",
#endif
                 nfc_handler_is_running() ? "Running" : "Stopped",
                 relay_control_is_active() ? "Active" : "Idle",
#ifdef CONFIG_CLOSEDGATE_ENROLL_ENABLED
                 s_enroll_mode ? "ACTIVE" : "Off"
#else
                 "N/A"
#endif
                 );
        
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

#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_CARD_SOUND)
    buzzer_play_pattern(BUZZER_PATTERN_CARD_DETECTED);
#endif

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

    /* Print UID in hex for debugging/manual use */
    char uid_hex[15] = {0};
    for (size_t i = 0; i < uid_len && i < 7; i++) {
        sprintf(&uid_hex[i * 2], "%02X", uid[i]);
    }
    ESP_LOGI(TAG, "UID received from NFC: %s (len=%d)", uid_hex, (int)uid_len);

#ifdef CONFIG_CLOSEDGATE_ENROLL_ENABLED
    /* ENROLL MODE: add card to whitelist instead of checking */
    if (s_enroll_mode) {
        s_enroll_mode = false;  /* One-shot: exit enroll after one card */

        esp_err_t ret = mifare_auth_add_uid(uid, uid_len);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, ">>> ENROLLED: UID %s added to whitelist (%d total) <<<",
                     uid_hex, mifare_auth_get_uid_count());
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ACCESS_SOUND)
            /* Double beep = enrolled successfully */
            buzzer_play_pattern(BUZZER_PATTERN_ACCESS_GRANTED);
            vTaskDelay(pdMS_TO_TICKS(300));
            buzzer_play_pattern(BUZZER_PATTERN_ACCESS_GRANTED);
#endif
        } else {
            ESP_LOGE(TAG, ">>> ENROLL FAILED: %s <<<", esp_err_to_name(ret));
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ERROR_SOUND)
            buzzer_play_pattern(BUZZER_PATTERN_ERROR);
#endif
        }
        return;
    }
#endif /* CONFIG_CLOSEDGATE_ENROLL_ENABLED */

    if (mifare_auth_check_uid(uid, uid_len)) {
        ESP_LOGI(TAG, "=== ACCESS GRANTED (MIFARE) ===");
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ACCESS_SOUND)
        buzzer_play_pattern(BUZZER_PATTERN_ACCESS_GRANTED);
#endif
        relay_control_trigger();
    } else {
        ESP_LOGW(TAG, "=== ACCESS DENIED: UID %s not in whitelist ===", uid_hex);
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ACCESS_SOUND)
        buzzer_play_pattern(BUZZER_PATTERN_ACCESS_DENIED);
#endif
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
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ACCESS_SOUND)
                    buzzer_play_pattern(BUZZER_PATTERN_ACCESS_GRANTED);
#endif
                    relay_control_trigger();
                    break;

                case YUBIKEY_STATUS_REPLAYED_OTP:
                    ESP_LOGW(TAG, "=== ACCESS DENIED: Replayed OTP ===");
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ACCESS_SOUND)
                    buzzer_play_pattern(BUZZER_PATTERN_ACCESS_DENIED);
#endif
                    break;

                case YUBIKEY_STATUS_BAD_OTP:
                    ESP_LOGW(TAG, "=== ACCESS DENIED: Invalid OTP format ===");
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ACCESS_SOUND)
                    buzzer_play_pattern(BUZZER_PATTERN_ACCESS_DENIED);
#endif
                    break;

                case YUBIKEY_STATUS_NO_SUCH_CLIENT:
                    ESP_LOGE(TAG, "=== ERROR: Invalid Yubico Client ID ===");
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ERROR_SOUND)
                    buzzer_play_pattern(BUZZER_PATTERN_ERROR);
#endif
                    break;

                case YUBIKEY_STATUS_NETWORK_ERROR:
                    ESP_LOGE(TAG, "=== ERROR: Network communication failed ===");
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ERROR_SOUND)
                    buzzer_play_pattern(BUZZER_PATTERN_ERROR);
#endif
                    break;

                default:
                    ESP_LOGW(TAG, "=== ACCESS DENIED: %s ===", 
                             yubikey_status_to_string(status));
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_ACCESS_SOUND)
                    buzzer_play_pattern(BUZZER_PATTERN_ACCESS_DENIED);
#endif
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
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_WIFI_SOUND)
            buzzer_play_pattern(BUZZER_PATTERN_WIFI_CONNECTED);
#endif
            break;
        case WIFI_STATE_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi: Disconnected");
            break;
        case WIFI_STATE_CONNECTING:
            ESP_LOGI(TAG, "WiFi: Connecting...");
            break;
        case WIFI_STATE_FAILED:
            ESP_LOGE(TAG, "WiFi: Connection failed");
#if defined(CONFIG_CLOSEDGATE_BUZZER_ENABLED) && defined(CONFIG_CLOSEDGATE_BUZZER_WIFI_SOUND)
            buzzer_play_pattern(BUZZER_PATTERN_WIFI_DISCONNECTED);
#endif
            break;
    }
}
#endif /* CONFIG_CLOSEDGATE_YUBIKEY_ENABLED */
