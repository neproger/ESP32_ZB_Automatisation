#pragma once

/*
 * Иконки погоды (40×40 ARGB8888, LVGL 9), сгенерированы из иконок v1
 * (tools/downscale_icons.py). condition → иконка — weather_icon_for().
 */

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t wicon_sunny;
extern const lv_image_dsc_t wicon_clear_night;
extern const lv_image_dsc_t wicon_partlycloudy;
extern const lv_image_dsc_t wicon_cloudy;
extern const lv_image_dsc_t wicon_overcast;
extern const lv_image_dsc_t wicon_fog;
extern const lv_image_dsc_t wicon_rainy;
extern const lv_image_dsc_t wicon_pouring;
extern const lv_image_dsc_t wicon_snowy;
extern const lv_image_dsc_t wicon_snowy_rainy;
extern const lv_image_dsc_t wicon_hail;
extern const lv_image_dsc_t wicon_lightning;
extern const lv_image_dsc_t wicon_lightning_rainy;
extern const lv_image_dsc_t wicon_windy;
extern const lv_image_dsc_t wicon_windy_variant;
extern const lv_image_dsc_t wicon_exceptional;

/* Иконка по значению ha_weather_condition_t; неизвестное — exceptional. */
const lv_image_dsc_t *weather_icon_for(uint8_t condition);

#ifdef __cplusplus
}
#endif
