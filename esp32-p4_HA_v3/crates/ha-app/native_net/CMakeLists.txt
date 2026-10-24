idf_component_register(SRCS "ha_wifi.c" "ha_store.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_partition)
