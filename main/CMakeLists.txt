# CLOSEDGATE Main Component CMakeLists.txt

idf_component_register(
    SRCS
        "closedgate_main.c"
        "nfc_handler.c"
        "yubikey_verify.c"
        "wifi_manager.c"
        "relay_control.c"
        "mifare_auth.c"
        "buzzer.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_client
        esp_tls
)
