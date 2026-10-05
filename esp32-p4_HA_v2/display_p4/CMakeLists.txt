# Порт Display под P4 (docs/clients/DISPLAY.md §6). ui/ (компонент `display`)
# collector'ит только LVGL/Domain/ha_model; esp_lcd/lvgl_port/GT911 живут здесь.
idf_component_register(
    SRCS
        "src/display_p4.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        display
        lvgl
        esp_lvgl_port
        esp_lcd_touch_gt911
    PRIV_REQUIRES
        esp_lcd
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_gpio
        esp_hw_support
)
