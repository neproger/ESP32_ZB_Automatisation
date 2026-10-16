#pragma once

#include <stdint.h>

#include "lvgl.h"

/*
 * Цветовые преобразования UI: sRGB ↔ HSV ↔ CIE xy (для MoveToColor) и упаковка в
 * lv_color_t. Вынесено из ui_widgets, чтобы виджеты не смешивались с математикой цвета.
 */

lv_color_t ui_color_from_rgb(uint8_t r, uint8_t g, uint8_t b);
lv_color_t ui_color_from_hs(uint16_t hue, uint8_t sat);

float ui_color_clamp01f(float v);

void ui_color_rgb8_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8, float *h, float *s, float *v);
void ui_color_hsv_to_rgb8(float h, float s, float v, uint8_t *r8, uint8_t *g8, uint8_t *b8);
void ui_color_rgb8_to_xy(uint8_t r8, uint8_t g8, uint8_t b8, uint16_t *x_out, uint16_t *y_out);
void ui_color_xy_to_rgb8(uint16_t x16, uint16_t y16, uint8_t *r8, uint8_t *g8, uint8_t *b8);
