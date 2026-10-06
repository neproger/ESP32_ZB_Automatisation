#pragma once

/*
 * Экран настроек: читает/пишет одну запись HA_ENTITY_SETTINGS
 * (docs/services/SETTINGS.md). Владелец экрана/переходов — display.c.
 */

#include "lvgl.h"

#include "domain/domain.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ui_settings_back_cb)(void);
typedef void (*ui_settings_theme_cb)(void);

/*
 * back — возврат к экранам групп; theme_changed — тема записана, нужна пересборка UI
 * (цвета фиксируются при создании объектов, владелец экранов — display.c).
 */
lv_obj_t *ui_settings_create(domain_t *domain, ui_settings_back_cb back,
                             ui_settings_theme_cb theme_changed);
void ui_settings_apply(void);

#ifdef __cplusplus
}
#endif
