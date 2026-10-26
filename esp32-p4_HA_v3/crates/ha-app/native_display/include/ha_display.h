#pragma once

/*
 * Порт дисплея под Guition JC4880P443C_I_W (ESP32-P4):
 * ST7701 480x800 MIPI-DSI + GT911 touch + esp_lvgl_port.
 * Драйвер/настройки панели остаются здесь; LVGL-UI живёт на Rust.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Кириллические шрифты и иконочные наборы из V2 (LVGL 9.6, те же файлы). */
extern const lv_font_t ui_font_20;
extern const lv_font_t ui_font_32;
extern const lv_font_t ui_font_48;
extern const lv_font_t ui_icons;
extern const lv_font_t wicons;

/* Поднимает панель, тач и LVGL-порт (задача LVGL внутри esp_lvgl_port).
 * 0 — успех, иначе esp_err_t. */
int ha_display_start(void);

/* Mьютекс LVGL: любой вызов lv_* из Rust — только под этим lock. */
bool ha_display_lock(uint32_t timeout_ms);
void ha_display_unlock(void);

/* Подсветка 0..100 %; 0 — выключена. */
void ha_display_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif
