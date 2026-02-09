#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize LVGL port, display and optional touch input.
// Touch handle may be NULL; in that case LVGL input device is not created.
esp_err_t devices_lvgl_init(esp_lcd_touch_handle_t touch_handle);
// Deinitialize LVGL port and internal handles (idempotent).
esp_err_t devices_lvgl_deinit(void);

#ifdef __cplusplus
}
#endif
