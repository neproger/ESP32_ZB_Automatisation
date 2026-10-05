# Wi-Fi service: владеет радио на внешнем C3 (ESP-Hosted UART) и provisioning
# (docs/services/WIFI.md). Заполняет wifi_scan/known/status в Domain и исполняет
# HA_CMD_WIFI_SCAN / HA_CMD_WIFI_CONNECT.
idf_component_register(
    SRCS
        "src/wifi.c"
        "src/wifi_select.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        domain
        ha_model
        sys
    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_hosted
)
