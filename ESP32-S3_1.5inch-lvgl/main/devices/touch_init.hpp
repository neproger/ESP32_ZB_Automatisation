#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize CST816S touch controller and return handle.
esp_err_t devices_touch_init(esp_lcd_touch_handle_t *out_handle);
// Deinitialize touch controller and its I2C/IO resources (idempotent).
esp_err_t devices_touch_deinit(void);

#ifdef __cplusplus
}
#endif
