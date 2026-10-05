#pragma once

/*
 * Стиль и метрики под панель 480×800 портрет (ST7701, docs/hardware/JC4880P443C_I_W.md).
 * Только числа и шрифты — цвета живут в одном месте: ui_palette.h.
 */

#include "ui_fonts.h"
#include "ui_palette.h"

#define UI_SCREEN_W 480
#define UI_SCREEN_H 800

/* Высота строки состояния на lv_layer_top(); экраны резервируют её сверху. */
#define UI_STATUSBAR_H 56

#define UI_FONT_HEADER (&ui_font_32) /* заголовок экрана (группы) */
#define UI_FONT_TITLE  (&ui_font_32)
#define UI_FONT_SUB    (&ui_font_20)
#define UI_FONT_BODY   (&ui_font_20)
#define UI_FONT_VALUE  (&ui_font_48)

/* Высота полосы навигационных точек внизу (на lv_layer_top). */
#define UI_NAV_DOTS_H 40
