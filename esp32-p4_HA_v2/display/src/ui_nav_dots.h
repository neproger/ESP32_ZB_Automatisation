#pragma once

/*
 * Навигационные точки (карусель) на lv_layer_top(): число экранов = число групп,
 * активная отмечена. Живут вне screen, поэтому не уезжают при переходах.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_nav_dots_create(void);
void ui_nav_dots_update(size_t count, size_t active);

#ifdef __cplusplus
}
#endif
