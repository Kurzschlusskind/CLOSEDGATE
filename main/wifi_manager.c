/**
 * @file wifi_manager.c
 * @brief WiFi Manager Implementation with automatic reconnection
 * 
 * Implements WiFi STA mode with event-based connection management
 * and exponential backoff for reconnection attempts.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "wifi_manager.h"
#include "sdkconfig.h"

static const char *TAG = "CLOSEDGATE";

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* Module state */
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static wifi_state_t s_wifi_state = WIFI_STATE_DISCONNECTED;
static wifi_state_callback_t s_state_callback = NULL;
static int s_retry_count = 0;
static bool s_initialized = false;

/* Forward declarations */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
static void set_state(wifi_state_t new_state);

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WiFi manager");

    /* Create event group */
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Create default WiFi station */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default STA netif");
        vEventGroupDelete(s_wifi_event_group);
        return ESP_FAIL;
    }

    /* Initialize WiFi with default config */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, NULL));

    /* Set WiFi mode to station */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized");
    
    return ESP_OK;
}

esp_err_t wifi_manager_start(const char *ssid, const char *password,
                             wifi_state_callback_t callback)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting WiFi connection to SSID: %s", ssid);

    s_state_callback = callback;
    s_retry_count = 0;

    /* Configure WiFi */
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)wifi_config.sta.password, password, 
                sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    set_state(WIFI_STATE_CONNECTING);
    
    /* Start WiFi */
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        set_state(WIFI_STATE_FAILED);
        return ret;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (!s_initialized || s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Waiting for WiFi connection (timeout=%"PRIu32"ms)", timeout_ms);

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        ticks
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "WiFi connection timeout");
    return ESP_ERR_TIMEOUT;
}

wifi_state_t wifi_manager_get_state(void)
{
    return s_wifi_state;
}

bool wifi_manager_is_connected(void)
{
    return s_wifi_state == WIFI_STATE_CONNECTED;
}

void wifi_manager_stop(void)
{
    if (!s_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Stopping WiFi");
    esp_wifi_stop();
    set_state(WIFI_STATE_DISCONNECTED);
    
    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
}

void wifi_manager_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    wifi_manager_stop();
    
    esp_wifi_deinit();
    
    if (s_sta_netif != NULL) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
    
    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_initialized = false;
    s_state_callback = NULL;
    
    ESP_LOGI(TAG, "WiFi manager deinitialized");
}

/* ============ Private Functions ============ */

static void set_state(wifi_state_t new_state)
{
    if (s_wifi_state != new_state) {
        ESP_LOGI(TAG, "WiFi state: %d -> %d", s_wifi_state, new_state);
        s_wifi_state = new_state;
        
        if (s_state_callback != NULL) {
            s_state_callback(new_state);
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started, connecting...");
            esp_wifi_connect();
            break;
            
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *event = 
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi disconnected, reason: %d", event->reason);
            
            set_state(WIFI_STATE_DISCONNECTED);
            
            int max_retries = CONFIG_CLOSEDGATE_WIFI_RECONNECT_MAX_RETRY;
            if (s_retry_count < max_retries) {
                s_retry_count++;
                
                /* Exponential backoff: 1s, 2s, 4s, 8s, ... (max 30s) */
                int delay_ms = (1 << (s_retry_count - 1)) * 1000;
                if (delay_ms > 30000) delay_ms = 30000;
                
                ESP_LOGI(TAG, "Reconnecting in %d ms (attempt %d/%d)", 
                         delay_ms, s_retry_count, max_retries);
                         
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
                set_state(WIFI_STATE_CONNECTING);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "Max reconnection attempts reached");
                set_state(WIFI_STATE_FAILED);
                if (s_wifi_event_group != NULL) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
            }
            break;
        }
        
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected to AP");
            s_retry_count = 0;  /* Reset retry counter */
            break;
            
        default:
            break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        
        set_state(WIFI_STATE_CONNECTED);
        
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}
