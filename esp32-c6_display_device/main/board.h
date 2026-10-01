#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* Waveshare ESP32-C6-LCD-1.47 board pin map. */

/* LCD (ST7789, SPI2, portrait 172x320) */
#define BOARD_LCD_HOST           SPI2_HOST
#define BOARD_LCD_PIXEL_CLK_HZ   (12 * 1000 * 1000)
#define BOARD_LCD_SCLK           GPIO_NUM_7
#define BOARD_LCD_MOSI           GPIO_NUM_6
#define BOARD_LCD_MISO           GPIO_NUM_NC
#define BOARD_LCD_CS             GPIO_NUM_14
#define BOARD_LCD_DC             GPIO_NUM_15
#define BOARD_LCD_RST            GPIO_NUM_21
#define BOARD_LCD_BL             GPIO_NUM_22
#define BOARD_LCD_H_RES          172
#define BOARD_LCD_V_RES          320
#define BOARD_LCD_X_GAP          34
#define BOARD_LCD_Y_GAP          0

/* On-board addressable RGB LED (WS2812) */
#define BOARD_RGB_LED_GPIO       GPIO_NUM_8

/* On-board BOOT button (active-low) */
#define BOARD_BUTTON_GPIO        GPIO_NUM_9
#define BOARD_BUTTON_ACTIVE_LEVEL 0
