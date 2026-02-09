#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_lcd_touch.h"
#include <stdbool.h>
#include <stdint.h>

// Board input pin assignment (shared between devices_init and UI).
// User button (BOOT)
#define BSP_BTN_PRESS GPIO_NUM_0
// Encoder phases
#define BSP_ENCODER_A GPIO_NUM_6
#define BSP_ENCODER_B GPIO_NUM_5
// Optional wakeup GPIO for light/deep sleep (use BOOT button GPIO0)
#define BSP_WAKE_GPIO GPIO_NUM_0

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize display, touch, LVGL port, encoder and button */
esp_err_t devices_init(void);

// Enable or disable LCD panel output (DCS DISPLAY_ON/OFF).
esp_err_t devices_display_set_enabled(bool enabled);

// Set/get backlight brightness in 0..255 range.
esp_err_t devices_display_set_brightness(uint8_t level);
esp_err_t devices_display_get_brightness(uint8_t *out_level);

// Touch controller handle helpers.
esp_lcd_touch_handle_t devices_get_touch_handle(void);
bool devices_has_touch(void);

// Per-device init/deinit API (idempotent)
esp_err_t devices_display_init(void);
esp_err_t devices_display_deinit(void);

esp_err_t devices_touch_init(esp_lcd_touch_handle_t *out_handle);
esp_err_t devices_touch_deinit(void);

esp_err_t devices_lvgl_init(esp_lcd_touch_handle_t touch_handle);
esp_err_t devices_lvgl_deinit(void);

esp_err_t devices_input_init(void);
esp_err_t devices_input_deinit(void);

#ifdef __cplusplus
}
#endif

