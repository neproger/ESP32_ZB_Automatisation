#pragma once

#include <stdint.h>
#include "esp_err.h"

/* On-board RGB LED used as the "lamp" of the Zigbee light endpoint. */

esp_err_t rgb_led_init(void);

/* Turn the lamp fully off. */
void rgb_led_off(void);

/* Hue/Saturation mode. hue: 0..254, sat: 0..254, level: 0..254 (ZCL units). */
void rgb_led_set_hs(uint8_t hue, uint8_t sat, uint8_t level);

/* CIE xy mode. x/y: 0..65535 (ZCL units), level: 0..254. */
void rgb_led_set_xy(uint16_t x, uint16_t y, uint8_t level);
