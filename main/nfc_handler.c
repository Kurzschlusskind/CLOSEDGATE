/**
 * @file nfc_handler.c
 * @brief PN532 NFC Handler Implementation
 * 
 * Implements I2C communication with PN532 NFC reader for YubiKey OTP reading.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "nfc_handler.h"
#include "sdkconfig.h"

static const char *TAG = "CLOSEDGATE";

/* I2C configuration */
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_MASTER_FREQ_HZ      100000
#define I2C_MASTER_TIMEOUT_MS   1000

/* PN532 Commands */
#define PN532_PREAMBLE          0x00
#define PN532_STARTCODE1        0x00
#define PN532_STARTCODE2        0xFF
#define PN532_POSTAMBLE         0x00
#define PN532_HOSTTOPN532       0xD4
#define PN532_PN532TOHOST       0xD5

/* PN532 Command codes */
#define PN532_CMD_GETFIRMWAREVERSION    0x02
#define PN532_CMD_SAMCONFIGURATION      0x14
#define PN532_CMD_INLISTPASSIVETARGET   0x4A
#define PN532_CMD_INDATAEXCHANGE        0x40

/* PN532 Response codes */
#define PN532_RSP_INDATAEXCHANGE        0x41
#define PN532_RSP_INLISTPASSIVETARGET   0x4B

/* ISO14443A Commands */
#define ISO14443A_CMD_RATS              0xE0

/* NDEF constants */
#define NDEF_TNF_WELL_KNOWN             0x01
#define NDEF_RTD_TEXT                   'T'

/* Task configuration */
#define NFC_TASK_STACK_SIZE     4096
#define NFC_TASK_PRIORITY       5

/* Module state */
static bool s_initialized = false;
static bool s_running = false;
static TaskHandle_t s_nfc_task_handle = NULL;
static nfc_otp_callback_t s_otp_callback = NULL;
static nfc_uid_callback_t s_uid_callback = NULL;
static int s_sda_gpio = -1;
static int s_scl_gpio = -1;

/* Forward declarations */
static void nfc_polling_task(void *pvParameters);
static esp_err_t pn532_write_command(const uint8_t *cmd, size_t cmd_len);
static esp_err_t pn532_read_response(uint8_t *resp, size_t *resp_len, uint32_t timeout_ms);
static esp_err_t pn532_send_command_check_ack(const uint8_t *cmd, size_t cmd_len);
static esp_err_t pn532_get_firmware_version(uint32_t *version);
static esp_err_t pn532_sam_configuration(void);
static esp_err_t pn532_rf_configuration_retries(void);
static esp_err_t pn532_read_passive_target(uint8_t *uid, size_t *uid_len);
static esp_err_t pn532_in_data_exchange(const uint8_t *send, size_t send_len,
                                        uint8_t *response, size_t *response_len);
static int parse_ndef_text_record(const uint8_t *data, size_t len, 
                                  char *text_out, size_t text_max_len);

esp_err_t nfc_handler_init(int sda_gpio, int scl_gpio)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "NFC handler already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing NFC handler (SDA=%d, SCL=%d)", sda_gpio, scl_gpio);

    s_sda_gpio = sda_gpio;
    s_scl_gpio = scl_gpio;

    /* Configure I2C master */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Wait for PN532 to boot */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Get firmware version to verify communication */
    uint32_t version = 0;
    ret = pn532_get_firmware_version(&version);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get PN532 firmware version");
        i2c_driver_delete(I2C_MASTER_NUM);
        return ret;
    }

    ESP_LOGI(TAG, "PN532 firmware version: 0x%08" PRIx32, version);

    /* Configure SAM (Security Access Module) */
    ret = pn532_sam_configuration();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PN532 SAM");
        i2c_driver_delete(I2C_MASTER_NUM);
        return ret;
    }

    /* Set MxRtyPassiveActivation=0: InListPassiveTarget returns immediately
     * when no card is in field instead of blocking until one appears. */
    ret = pn532_rf_configuration_retries();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RF MaxRetries config failed (non-fatal): %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "PN532 initialized successfully");
    s_initialized = true;
    return ESP_OK;
}

esp_err_t nfc_handler_start(nfc_otp_callback_t otp_callback, nfc_uid_callback_t uid_callback)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "NFC handler not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        ESP_LOGW(TAG, "NFC handler already running");
        return ESP_OK;
    }

    if (otp_callback == NULL && uid_callback == NULL) {
        ESP_LOGE(TAG, "At least one callback must be non-NULL");
        return ESP_ERR_INVALID_ARG;
    }

    s_otp_callback = otp_callback;
    s_uid_callback = uid_callback;

    /* Set s_running BEFORE xTaskCreate to avoid race condition:
     * NFC task has higher priority and runs immediately, checking s_running */
    s_running = true;

    BaseType_t ret = xTaskCreate(
        nfc_polling_task,
        "nfc_poll",
        NFC_TASK_STACK_SIZE,
        NULL,
        NFC_TASK_PRIORITY,
        &s_nfc_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create NFC polling task");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "NFC polling task started");
    return ESP_OK;
}

esp_err_t nfc_handler_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    s_running = false;
    
    if (s_nfc_task_handle != NULL) {
        /* Give task time to exit cleanly */
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(s_nfc_task_handle);
        s_nfc_task_handle = NULL;
    }

    ESP_LOGI(TAG, "NFC polling task stopped");
    return ESP_OK;
}

bool nfc_handler_is_running(void)
{
    return s_running;
}

void nfc_handler_deinit(void)
{
    nfc_handler_stop();

    if (s_initialized) {
        i2c_driver_delete(I2C_MASTER_NUM);
        s_initialized = false;
        s_otp_callback = NULL;
        s_uid_callback = NULL;
        ESP_LOGI(TAG, "NFC handler deinitialized");
    }
}

/* ============ Private Functions ============ */

static void nfc_polling_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t uid[10];
    size_t uid_len;
    char otp_buffer[NFC_OTP_MAX_LEN];
    uint32_t poll_interval = CONFIG_CLOSEDGATE_NFC_POLL_INTERVAL_MS;

    ESP_LOGI(TAG, "NFC polling task running (interval=%"PRIu32"ms)", poll_interval);

    uint32_t poll_count = 0;
    while (s_running) {
        uid_len = sizeof(uid);
        poll_count++;
        
        /* Try to detect ISO14443A target */
        esp_err_t ret = pn532_read_passive_target(uid, &uid_len);
        
        /* Only log unexpected errors, not the normal no-card case */
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "POLL #%"PRIu32": %s", poll_count, esp_err_to_name(ret));
        }
        
        if (ret == ESP_OK && uid_len > 0) {
            ESP_LOGI(TAG, "NFC tag detected, UID length: %d", (int)uid_len);
            
            /* Try to read NDEF data from tag */
            uint8_t ndef_data[256];
            size_t ndef_len = sizeof(ndef_data);
            bool ndef_success = false;
            bool otp_found = false;
            
            /* Select NDEF application and read */
            /* For YubiKey, we need to select the NDEF application first */
            uint8_t select_ndef[] = {0x00, 0xA4, 0x04, 0x00, 0x07, 
                                     0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, 0x00};
            
            ret = pn532_in_data_exchange(select_ndef, sizeof(select_ndef), 
                                         ndef_data, &ndef_len);
            
            if (ret == ESP_OK && ndef_len >= 2) {
                /* Check for success status (90 00) */
                if (ndef_data[ndef_len-2] == 0x90 && ndef_data[ndef_len-1] == 0x00) {
                    ESP_LOGI(TAG, "NDEF application selected");
                    ndef_success = true;
                    
                    /* Select NDEF file (CC file first) */
                    uint8_t select_cc[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x03};
                    ndef_len = sizeof(ndef_data);
                    ret = pn532_in_data_exchange(select_cc, sizeof(select_cc),
                                                 ndef_data, &ndef_len);
                    
                    /* Select NDEF data file */
                    uint8_t select_ndef_file[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x04};
                    ndef_len = sizeof(ndef_data);
                    ret = pn532_in_data_exchange(select_ndef_file, sizeof(select_ndef_file),
                                                 ndef_data, &ndef_len);
                    
                    if (ret == ESP_OK) {
                        /* Read NDEF length */
                        uint8_t read_len[] = {0x00, 0xB0, 0x00, 0x00, 0x02};
                        ndef_len = sizeof(ndef_data);
                        ret = pn532_in_data_exchange(read_len, sizeof(read_len),
                                                     ndef_data, &ndef_len);
                        
                        if (ret == ESP_OK && ndef_len >= 4) {
                            uint16_t msg_len = (ndef_data[0] << 8) | ndef_data[1];
                            ESP_LOGI(TAG, "NDEF message length: %d", msg_len);
                            
                            if (msg_len > 0 && msg_len < 250) {
                                /* Read NDEF message */
                                uint8_t read_msg[] = {0x00, 0xB0, 0x00, 0x02, 
                                                     (uint8_t)(msg_len & 0xFF)};
                                ndef_len = sizeof(ndef_data);
                                ret = pn532_in_data_exchange(read_msg, sizeof(read_msg),
                                                             ndef_data, &ndef_len);
                                
                                if (ret == ESP_OK && ndef_len > 2) {
                                    /* Parse NDEF text record */
                                    int text_len = parse_ndef_text_record(
                                        ndef_data, ndef_len - 2,
                                        otp_buffer, sizeof(otp_buffer) - 1);
                                    
                                    if (text_len > 0) {
                                        otp_buffer[text_len] = '\0';
                                        ESP_LOGI(TAG, "OTP extracted: %s", otp_buffer);
                                        otp_found = true;
                                        
                                        if (s_otp_callback != NULL) {
                                            s_otp_callback(otp_buffer, text_len);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            /*
             * Fallback to UID callback when:
             *  - NDEF application selection failed (plain MIFARE Classic), OR
             *  - NDEF was selected but no OTP could be extracted
             */
            if ((!ndef_success || !otp_found) && s_uid_callback != NULL) {
                ESP_LOGI(TAG, "Using UID callback (ndef=%d, otp=%d)",
                         ndef_success, otp_found);
                s_uid_callback(uid, uid_len);
            }
            
            /* Wait before next poll to avoid reading same tag multiple times */
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        
        vTaskDelay(pdMS_TO_TICKS(poll_interval));
    }

    ESP_LOGI(TAG, "NFC polling task exiting");
    vTaskDelete(NULL);
}

static esp_err_t pn532_write_command(const uint8_t *cmd, size_t cmd_len)
{
    uint8_t frame[64];
    size_t frame_len = 0;
    
    /* Build PN532 frame */
    frame[frame_len++] = PN532_PREAMBLE;
    frame[frame_len++] = PN532_STARTCODE1;
    frame[frame_len++] = PN532_STARTCODE2;
    frame[frame_len++] = (uint8_t)(cmd_len + 1);           /* LEN */
    frame[frame_len++] = (uint8_t)(~(cmd_len + 1) + 1);    /* LCS */
    frame[frame_len++] = PN532_HOSTTOPN532;                /* TFI */
    
    uint8_t checksum = PN532_HOSTTOPN532;
    for (size_t i = 0; i < cmd_len; i++) {
        frame[frame_len++] = cmd[i];
        checksum += cmd[i];
    }
    
    frame[frame_len++] = (uint8_t)(~checksum + 1);         /* DCS */
    frame[frame_len++] = PN532_POSTAMBLE;
    
    esp_err_t ret = i2c_master_write_to_device(
        I2C_MASTER_NUM,
        PN532_I2C_ADDRESS,
        frame,
        frame_len,
        pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS)
    );
    
    return ret;
}

static esp_err_t pn532_read_response(uint8_t *resp, size_t *resp_len, uint32_t timeout_ms)
{
    uint8_t ready_byte;
    uint32_t start_time = xTaskGetTickCount();
    
    /* Wait for PN532 to be ready (RDY bit) */
    while (1) {
        esp_err_t ret = i2c_master_read_from_device(
            I2C_MASTER_NUM,
            PN532_I2C_ADDRESS,
            &ready_byte,
            1,
            pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS)
        );
        
        if (ret != ESP_OK) {
            return ret;
        }
        
        if (ready_byte & 0x01) {
            break;  /* PN532 is ready */
        }
        
        if ((xTaskGetTickCount() - start_time) > pdMS_TO_TICKS(timeout_ms)) {
            return ESP_ERR_TIMEOUT;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    /* Read response frame */
    uint8_t frame[64];
    esp_err_t ret = i2c_master_read_from_device(
        I2C_MASTER_NUM,
        PN532_I2C_ADDRESS,
        frame,
        sizeof(frame),
        pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS)
    );
    
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* Parse frame - skip ready byte at start */
    int idx = 0;
    if (frame[idx] == 0x01) idx++;  /* Skip RDY */
    if (frame[idx] != PN532_PREAMBLE) return ESP_ERR_INVALID_RESPONSE;
    idx++;
    if (frame[idx] != PN532_STARTCODE1) return ESP_ERR_INVALID_RESPONSE;
    idx++;
    if (frame[idx] != PN532_STARTCODE2) return ESP_ERR_INVALID_RESPONSE;
    idx++;
    
    uint8_t len = frame[idx++];
    uint8_t lcs = frame[idx++];
    
    if ((uint8_t)(len + lcs) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    
    if (frame[idx] != PN532_PN532TOHOST) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    idx++;
    
    /* Copy response data (excluding TFI) */
    size_t data_len = len - 1;
    if (data_len > *resp_len) {
        data_len = *resp_len;
    }
    
    memcpy(resp, &frame[idx], data_len);
    *resp_len = data_len;
    
    return ESP_OK;
}

static esp_err_t pn532_send_command_check_ack(const uint8_t *cmd, size_t cmd_len)
{
    esp_err_t ret = pn532_write_command(cmd, cmd_len);
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* Wait a bit for processing */
    vTaskDelay(pdMS_TO_TICKS(10));
    
    /* Read and verify ACK frame */
    uint8_t ack[16];
    esp_err_t read_ret = i2c_master_read_from_device(
        I2C_MASTER_NUM,
        PN532_I2C_ADDRESS,
        ack,
        sizeof(ack),
        pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS)
    );
    
    if (read_ret != ESP_OK) {
        return read_ret;
    }
    
    /* ACK frame: 00 00 FF 00 FF 00 */
    int idx = 0;
    if (ack[idx] == 0x01) idx++;  /* Skip RDY byte if present */
    
    static const uint8_t expected_ack[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    if (memcmp(&ack[idx], expected_ack, sizeof(expected_ack)) != 0) {
        ESP_LOGW(TAG, "ACK mismatch");
        /* Continue anyway, some PN532 modules behave differently */
    }
    
    return ESP_OK;
}

static esp_err_t pn532_get_firmware_version(uint32_t *version)
{
    uint8_t cmd[] = {PN532_CMD_GETFIRMWAREVERSION};
    
    esp_err_t ret = pn532_send_command_check_ack(cmd, sizeof(cmd));
    if (ret != ESP_OK) {
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
    
    uint8_t resp[16];
    size_t resp_len = sizeof(resp);
    ret = pn532_read_response(resp, &resp_len, 1000);
    if (ret != ESP_OK) {
        return ret;
    }
    
    if (resp_len < 5 || resp[0] != (PN532_CMD_GETFIRMWAREVERSION + 1)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    *version = ((uint32_t)resp[1] << 24) | ((uint32_t)resp[2] << 16) |
               ((uint32_t)resp[3] << 8) | resp[4];
    
    return ESP_OK;
}

static esp_err_t pn532_sam_configuration(void)
{
    /* SAMConfiguration: Normal mode, timeout 1s, NO IRQ (we poll via I2C) */
    uint8_t cmd[] = {PN532_CMD_SAMCONFIGURATION, 0x01, 0x14, 0x00};
    
    esp_err_t ret = pn532_send_command_check_ack(cmd, sizeof(cmd));
    if (ret != ESP_OK) {
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
    
    uint8_t resp[8];
    size_t resp_len = sizeof(resp);
    ret = pn532_read_response(resp, &resp_len, 1000);
    if (ret != ESP_OK) {
        return ret;
    }
    
    if (resp_len < 1 || resp[0] != (PN532_CMD_SAMCONFIGURATION + 1)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    return ESP_OK;
}

static esp_err_t pn532_read_passive_target(uint8_t *uid, size_t *uid_len)
{
    /* InListPassiveTarget: 1 target, 106 kbps ISO14443A */
    uint8_t cmd[] = {PN532_CMD_INLISTPASSIVETARGET, 0x01, 0x00};
    
    esp_err_t ret = pn532_send_command_check_ack(cmd, sizeof(cmd));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "InListPassiveTarget ACK failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    uint8_t resp[64];
    size_t resp_len = sizeof(resp);
    /* With MxRtyPassiveActivation=0 the PN532 returns within ~50ms.
     * 500ms is a generous upper bound. */
    ret = pn532_read_response(resp, &resp_len, 500);
    if (ret != ESP_OK) {
        return ret;
    }
    
    if (resp_len < 1 || resp[0] != PN532_RSP_INLISTPASSIVETARGET) {
        ESP_LOGW(TAG, "Bad response code: 0x%02X (expected 0x%02X)", 
                 resp_len > 0 ? resp[0] : 0, PN532_RSP_INLISTPASSIVETARGET);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    if (resp_len < 2 || resp[1] == 0) {
        /* No target found */
        *uid_len = 0;
        return ESP_ERR_NOT_FOUND;
    }
    
    /*
     * InListPassiveTarget response for ISO14443A:
     *   resp[0] = 0x4B (response code)
     *   resp[1] = NbTg (number of targets detected)
     *   resp[2] = Tg   (target number, 01)
     *   resp[3] = SENS_RES (ATQA) byte 1
     *   resp[4] = SENS_RES (ATQA) byte 2
     *   resp[5] = SEL_RES  (SAK)
     *   resp[6] = NFCIDLength
     *   resp[7..] = NFCID (UID bytes)
     */
    if (resp_len >= 8) {
        uint8_t sak = resp[5];
        size_t nfcid_len = resp[6];
        ESP_LOGI(TAG, "Card: ATQA=%02X%02X SAK=%02X UID_len=%d",
                 resp[3], resp[4], sak, (int)nfcid_len);
        
        if (nfcid_len == 0 || nfcid_len > 10) {
            ESP_LOGW(TAG, "Invalid NFCID length: %d", (int)nfcid_len);
            *uid_len = 0;
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (nfcid_len > *uid_len) {
            nfcid_len = *uid_len;
        }
        if (resp_len >= 7 + nfcid_len) {
            memcpy(uid, &resp[7], nfcid_len);
            *uid_len = nfcid_len;
        } else {
            ESP_LOGW(TAG, "Response too short for UID");
            *uid_len = 0;
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        ESP_LOGW(TAG, "Response too short: %d bytes", (int)resp_len);
        *uid_len = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    
    return ESP_OK;
}

static esp_err_t pn532_rf_configuration_retries(void)
{
    /* RFConfiguration CfgItem=0x05 (MaxRetries):
     * MxRtyATR=0xFF, MxRtyPSL=0x01, MxRtyPassiveActivation=0x00
     * With PassiveActivation=0, InListPassiveTarget does a single RF poll
     * and returns NbTg=0 immediately if no card is in field. */
    uint8_t cmd[] = {0x32, 0x05, 0xFF, 0x01, 0x00};

    esp_err_t ret = pn532_send_command_check_ack(cmd, sizeof(cmd));
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t resp[8];
    size_t resp_len = sizeof(resp);
    ret = pn532_read_response(resp, &resp_len, 1000);
    if (ret != ESP_OK) return ret;

    /* Response code is command + 1 = 0x33 */
    if (resp_len < 1 || resp[0] != 0x33) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "PN532 passive activation retries set to 0");
    return ESP_OK;
}

static esp_err_t pn532_in_data_exchange(const uint8_t *send, size_t send_len,
                                        uint8_t *response, size_t *response_len)
{
    uint8_t cmd[64];
    cmd[0] = PN532_CMD_INDATAEXCHANGE;
    cmd[1] = 0x01;  /* Target number */
    
    if (send_len > sizeof(cmd) - 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    memcpy(&cmd[2], send, send_len);
    
    esp_err_t ret = pn532_send_command_check_ack(cmd, send_len + 2);
    if (ret != ESP_OK) {
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    uint8_t resp[64];
    size_t resp_len = sizeof(resp);
    ret = pn532_read_response(resp, &resp_len, 1000);
    if (ret != ESP_OK) {
        return ret;
    }
    
    if (resp_len < 2 || resp[0] != PN532_RSP_INDATAEXCHANGE) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    /* Check error code */
    if (resp[1] != 0x00) {
        ESP_LOGW(TAG, "InDataExchange error: 0x%02X", resp[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    /* Copy response data */
    size_t data_len = resp_len - 2;
    if (data_len > *response_len) {
        data_len = *response_len;
    }
    
    memcpy(response, &resp[2], data_len);
    *response_len = data_len;
    
    return ESP_OK;
}

static int parse_ndef_text_record(const uint8_t *data, size_t len, 
                                  char *text_out, size_t text_max_len)
{
    if (len < 3) {
        return -1;
    }
    
    size_t idx = 0;
    
    /* NDEF record header */
    uint8_t header = data[idx++];
    uint8_t tnf = header & 0x07;
    bool sr = (header & 0x10) != 0;  /* Short Record */
    bool il = (header & 0x08) != 0;  /* ID Length present */
    
    if (tnf != NDEF_TNF_WELL_KNOWN) {
        ESP_LOGW(TAG, "Not a well-known NDEF record (TNF=%d)", tnf);
        return -1;
    }
    
    if (idx >= len) return -1;
    uint8_t type_len = data[idx++];
    
    uint32_t payload_len;
    if (sr) {
        if (idx >= len) return -1;
        payload_len = data[idx++];
    } else {
        if (idx + 4 > len) return -1;
        payload_len = ((uint32_t)data[idx] << 24) | ((uint32_t)data[idx+1] << 16) |
                      ((uint32_t)data[idx+2] << 8) | data[idx+3];
        idx += 4;
    }
    
    uint8_t id_len = 0;
    if (il) {
        if (idx >= len) return -1;
        id_len = data[idx++];
    }
    
    /* Check type is "T" (Text) */
    if (type_len != 1 || idx >= len || data[idx] != NDEF_RTD_TEXT) {
        ESP_LOGW(TAG, "Not a Text record");
        return -1;
    }
    idx++;
    
    /* Skip ID if present */
    idx += id_len;
    
    if (idx >= len) return -1;
    
    /* Text record payload */
    uint8_t status = data[idx++];
    uint8_t lang_len = status & 0x3F;
    
    /* Skip language code */
    idx += lang_len;
    
    /* Calculate text length */
    if (payload_len < 1 + lang_len) {
        return -1;
    }
    size_t text_len = payload_len - 1 - lang_len;
    
    if (idx + text_len > len) {
        text_len = len - idx;
    }
    
    if (text_len > text_max_len) {
        text_len = text_max_len;
    }
    
    memcpy(text_out, &data[idx], text_len);
    
    return (int)text_len;
}
