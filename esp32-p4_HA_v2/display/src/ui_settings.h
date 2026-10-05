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

lv_obj_t *ui_settings_create(domain_t *domain, ui_settings_back_cb back);
void ui_settings_apply(void);

#ifdef __cplusplus
}
#endif
