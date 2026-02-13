#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <stdint.h>

// LCD and SPI bus configuration
#define LCD_HOST (SPI2_HOST)
#define LCD_DRAW_BUFF_DOUBLE (0)
#define LCD_LVGL_AVOID_TEAR (1)
#define LCD_DRAW_BUFF_HEIGHT (16)
#define LCD_SPI_SPEED_MHZ (40)
#define LCD_TRANS_QUEUE_DEPTH (10)
#define LCD_BK_LIGHT_ON_LEVEL 1
#define LCD_BK_LIGHT_OFF_LEVEL (!LCD_BK_LIGHT_ON_LEVEL)
#define PIN_LCD_CS (GPIO_NUM_12)
#define PIN_LCD_PCLK (GPIO_NUM_10)
#define PIN_LCD_DATA0 (GPIO_NUM_13)
#define PIN_LCD_DATA1 (GPIO_NUM_11)
#define PIN_LCD_DATA2 (GPIO_NUM_14)
#define PIN_LCD_DATA3 (GPIO_NUM_9)
#define PIN_LCD_RST (GPIO_NUM_8)
#define PIN_BK_LIGHT (GPIO_NUM_17)

#define LCD_H_RES 472
#define LCD_V_RES 466

#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BIT_PER_PIXEL (24)
#elif CONFIG_LV_COLOR_DEPTH == 16
#define LCD_BIT_PER_PIXEL (16)
#else
#define LCD_BIT_PER_PIXEL (16)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Initialize display: SPI/QSPI bus, SH8601 panel and backlight.
esp_err_t devices_display_init(void);
// Deinitialize display and backlight (idempotent).
esp_err_t devices_display_deinit(void);

// Accessors used by LVGL and power control.
esp_lcd_panel_io_handle_t devices_display_get_panel_io(void);
esp_lcd_panel_handle_t devices_display_get_panel(void);

// Set/get backlight brightness in 0..255 range (8-bit LEDC duty).
esp_err_t devices_display_set_brightness(uint8_t level);
esp_err_t devices_display_get_brightness(uint8_t *out_level);

#ifdef __cplusplus
}
#endif



