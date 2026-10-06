#pragma once

/*
 * Виджет одного состояния: group_item ссылается на состояние ключом Domain, а UI
 * выбирает форму виджета по (cluster_id, attr_id) и показывает значение из записи
 * состояния. Форма — проекция, а не хранимая сущность (docs/clients/DISPLAY.md §4).
 *
 * Цвет — составной (hue + saturation), поэтому его виджету нужен доступ к Domain,
 * чтобы прочитать соседние атрибуты кластера Color.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "domain/domain.h"
#include "ha_model/ha_entities.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_WIDGET_NONE = 0,
    UI_WIDGET_SWITCH,     /* OnOff: состояние on/off */
    UI_WIDGET_LEVEL,      /* Level: уровень 0..254 */
    UI_WIDGET_COLOR,      /* Color: свотч + hue/saturation */
    UI_WIDGET_COLOR_TEMP, /* Color: цветовая температура (mireds) */
    UI_WIDGET_INDICATOR,  /* бинарное состояние (занято/тревога) — только чтение */
    UI_WIDGET_VALUE,      /* числовое значение с единицей (датчик/батарея) */
} ui_widget_kind_t;

ui_widget_kind_t ui_widget_kind_for(uint16_t cluster_id, uint16_t attr_id);

/* Слайдер в общем стиле UI (тонкая дорожка + круглый маркер). */
lv_obj_t *ui_slider_create_styled(lv_obj_t *parent, int min, int max);

/* Выпадающий список в общем стиле UI (стрелка — глиф ui_icons, не встроенный символ). */
lv_obj_t *ui_dropdown_create_styled(lv_obj_t *parent);

typedef struct ui_widget ui_widget_t;

/*
 * Виджет владеет своими lv_obj и освобождает обёртку при удалении объекта (то есть при
 * удалении экрана/карточки). Отдельного destroy нет: удаляется lv_obj владельца.
 */
ui_widget_t *ui_widget_create(domain_t *domain, lv_obj_t *parent,
                              const ha_zb_state_key_t *state);
void ui_widget_apply(ui_widget_t *widget, const ha_zb_state_record_t *record, bool present);

#ifdef __cplusplus
}
#endif
