# Display — клиент Domain (docs/clients/DISPLAY.md). ui/ зависит только от LVGL,
# Domain API и форм ha_model; ESP-IDF (esp_lcd/lvgl_port) — отдельный port/.
# Иконки погоды — сгенерированные ассеты (по файлу на условие), glob оправдан.
file(GLOB WEATHER_ICON_SRCS "icons/wicon_*.c")

idf_component_register(
    SRCS
        "src/display.c"
        "src/ui_menu.c"
        "src/ui_nav_dots.c"
        "src/ui_page.c"
        "src/ui_settings.c"
        "src/ui_status_bar.c"
        "src/ui_wifi.c"
        "src/ui_widgets.c"
        "fonts/ui_font_20.c"
        "fonts/ui_font_32.c"
        "fonts/ui_font_48.c"
        "icons/weather_icons.c"
        ${WEATHER_ICON_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
        "icons"
    REQUIRES
        lvgl
        domain
        ha_model
        sys
)
