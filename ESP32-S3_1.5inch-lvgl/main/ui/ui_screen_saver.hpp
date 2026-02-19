#pragma once

#include "lvgl.h"

typedef void (*ui_screen_saver_wakeup_cb_t)(void);

void ui_screen_saver_init(lv_obj_t *root, ui_screen_saver_wakeup_cb_t wakeup_cb);
void ui_screen_saver_show(bool show);
bool ui_screen_saver_is_visible(void);
void ui_screen_saver_tick(void);
