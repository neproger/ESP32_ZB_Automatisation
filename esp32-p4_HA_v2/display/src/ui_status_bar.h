#pragma once

/*
 * Строка состояния на lv_layer_top(): живёт независимо от текущего экрана, поэтому
 * видна при переключении групп. Показывает локальное время и город системного
 * устройства (docs/services/SYSTEM.md). Данные — из Domain; логики здесь нет.
 */

#include "domain/domain.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_status_bar_create(domain_t *domain);
void ui_status_bar_apply(void);

#ifdef __cplusplus
}
#endif
