#pragma once

/*
 * Единственное место цветов UI. Активная палитра выбирается в рантайме
 * (`ui_palette_set`, хранится в settings.palette_id) — поэтому роли UI_COL_* это
 * доступ к текущей палитре, а не compile-time значения. Пресеты — в ui_palette.c.
 *
 * Роли:
 *   UI_COL_BG      фон экрана
 *   UI_COL_CARD    поверхности (карточки, меню, чипы-кнопки, строка состояния)
 *   UI_COL_TEXT    основной текст
 *   UI_COL_MUTED   вторичный текст
 *   UI_COL_ACCENT  акцент (активные элементы, кнопки действия, слайдер)
 *   UI_COL_OK      «нормально/занято» (индикаторы)
 *   UI_COL_DANGER  «тревога» (индикаторы)
 *   UI_COL_CHIP    неактивный чип/точка карусели
 *
 * Выбор темы меняет цвета только у вновь созданных объектов (LVGL «запекает» цвет в
 * момент создания), поэтому display после смены пересобирает дерево экранов.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PALETTE_RETRO = 0, /* чёрный/тёмно-синий/оранжевый/серый — по умолчанию */
    UI_PALETTE_WARM = 1,
    UI_PALETTE_DARK = 2,
    UI_PALETTE_SUMMER = 3, /* светлая: жёлтый/бирюзовый */
    UI_PALETTE_WINTER = 4, /* тёмная: зелёный/мята */
    UI_PALETTE_COUNT,
} ui_palette_id_t;

typedef enum {
    UI_ROLE_BG = 0,
    UI_ROLE_CARD,
    UI_ROLE_TEXT,
    UI_ROLE_MUTED,
    UI_ROLE_ACCENT,
    UI_ROLE_OK,
    UI_ROLE_DANGER,
    UI_ROLE_CHIP,
    UI_ROLE_COUNT,
} ui_palette_role_t;

/* Цвет роли активной палитры (0xRRGGBB). */
uint32_t ui_palette_color(ui_palette_role_t role);

ui_palette_id_t ui_palette_id(void);
void ui_palette_set(ui_palette_id_t id);

/* Человекочитаемое имя темы (для выбора в настройках). */
const char *ui_palette_name(ui_palette_id_t id);

#define UI_COL_BG     ui_palette_color(UI_ROLE_BG)
#define UI_COL_CARD   ui_palette_color(UI_ROLE_CARD)
#define UI_COL_TEXT   ui_palette_color(UI_ROLE_TEXT)
#define UI_COL_MUTED  ui_palette_color(UI_ROLE_MUTED)
#define UI_COL_ACCENT ui_palette_color(UI_ROLE_ACCENT)
#define UI_COL_OK     ui_palette_color(UI_ROLE_OK)
#define UI_COL_DANGER ui_palette_color(UI_ROLE_DANGER)
#define UI_COL_CHIP   ui_palette_color(UI_ROLE_CHIP)

#ifdef __cplusplus
}
#endif
