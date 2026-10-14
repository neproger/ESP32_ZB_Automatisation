#pragma once

/*
 * Один экран = одна группа: шапка с названием и ниже скроллируемый список плиток.
 * Каждая плитка — логическая Entity (endpoint), внутри — по виджету на её capability
 * (docs/clients/DISPLAY.md). Экран сам себя освобождает при удалении; переходы между
 * группами делаются снаружи через lv_screen_load_anim.
 */

#include <stddef.h>

#include "lvgl.h"

#include "domain/domain.h"
#include "ha_model/ha_entity.h"
#include "ha_model/ha_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title;      /* подпись плитки (копируется в label) */
    ha_entity_id_t entity;  /* плитка = Entity; виджеты — по её capability */
} ui_page_item_t;

typedef struct ui_page ui_page_t;

/* Жест на экране — наружу (навигация между группами живёт в display.c). */
typedef void (*ui_page_nav_cb)(void *ctx, lv_dir_t dir);

ui_page_t *ui_page_create(domain_t *domain, const char *group_title,
                          const ui_page_item_t *items, size_t item_count, ui_page_nav_cb nav,
                          void *nav_ctx);

/* Прочитать состояния всех виджетов и применить. */
void ui_page_apply(ui_page_t *page);

/*
 * Применить одно canonical-состояние (по факту Domain): виджетам экрана с этим
 * (entity, property); цветные виджеты — также по X/Y/яркости.
 */
void ui_page_apply_key(ui_page_t *page, const ha_state_key_t *key, const ha_value_t *value,
                       bool present);

lv_obj_t *ui_page_root(const ui_page_t *page);

#ifdef __cplusplus
}
#endif
