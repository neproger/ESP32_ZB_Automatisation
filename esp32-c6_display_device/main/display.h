#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ST7789 panel + LVGL (via esp_lvgl_port). */

esp_err_t display_init(void);

/* Thread-safe LVGL access for UI updates from other tasks. */
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

/* Backlight brightness 0..100. */
void display_set_backlight(uint8_t percent);
