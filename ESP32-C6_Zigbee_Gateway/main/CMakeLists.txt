idf_component_register(SRCS "esp_zigbee_gateway.c"
                            "gw_uart_link.c"
                            "gw_wifi_sta.c"
                            "gw_cloud_sync.c"
                    PRIV_REQUIRES ieee802154 esp_driver_usb_serial_jtag esp_netif
                                  vfs nvs_flash esp_driver_uart esp_timer
                                  esp_rom esp_system esp_hw_support
                                  esp_wifi
                                  gw_zigbee
                                  gw_core
                    INCLUDE_DIRS ".")
