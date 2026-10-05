#pragma once

/*
 * Пины/тайминги платы Guition JC4880P443C_I_W (JC-ESP32P4-M3), проверенные на
 * железе. Источник: https://github.com/ultramcu/guition-jc4880p4-bsp (MIT) —
 * пины и данные панели. Значения совпадают с docs/hardware/JC4880P443C_I_W.md
 * (кроме тач-RST: на этой плате он GPIO3, а не 22).
 */

#define BOARD_P4_LCD_H_RES 480
#define BOARD_P4_LCD_V_RES 800
#define BOARD_P4_LCD_DPI_CLK_MHZ 34

#define BOARD_P4_LCD_RST 5  /* panel reset (low 20ms, high 120ms) */
#define BOARD_P4_LCD_BL 23  /* backlight, LEDC PWM (feeds MP3202 boost) */

#define BOARD_P4_DSI_LDO_CHAN 3
#define BOARD_P4_DSI_LDO_MV 2500

#define BOARD_P4_TOUCH_SDA 7
#define BOARD_P4_TOUCH_SCL 8
#define BOARD_P4_TOUCH_RST 3
#define BOARD_P4_TOUCH_INT -1 /* -1 = polling (community docs list GPIO21) */
