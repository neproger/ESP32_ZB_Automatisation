idf_component_register(
    SRCS
        "app_main.c"
        "display.c"
        "ui.c"
        "rgb_led.c"
        "button.c"
        "zb_app.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_timer
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_lcd
)
