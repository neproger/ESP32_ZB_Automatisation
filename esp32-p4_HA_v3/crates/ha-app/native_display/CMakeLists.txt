idf_component_register(SRCS "ha_display.c"
                            "fonts/ui_font_20.c"
                            "fonts/ui_font_32.c"
                            "fonts/ui_font_48.c"
                            "icons/ui_icons.c"
                            "icons/wicons.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "." "fonts" "icons"
                       REQUIRES lvgl esp_lvgl_port esp_lcd_touch_gt911
                       PRIV_REQUIRES esp_lcd esp_driver_i2c esp_driver_ledc esp_driver_gpio esp_hw_support)
