#pragma once

/*
 * Кириллические шрифты UI (сгенерированы lv_font_conv из arial.ttf, диапазоны
 * ASCII + Latin-1 °, тире, №, кириллица U+0400..U+04FF). Встроенные шрифты LVGL
 * кириллицу не содержат, поэтому свои.
 */

#include "lvgl.h"

LV_FONT_DECLARE(ui_font_20);
LV_FONT_DECLARE(ui_font_32);
LV_FONT_DECLARE(ui_font_48);
