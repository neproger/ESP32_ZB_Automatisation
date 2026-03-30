#pragma once

#include "lvgl.h"

void ui_screen_devices_init(lv_obj_t *root);
void ui_screen_devices_render_if_needed(void);
bool ui_screen_devices_next_group(void);
bool ui_screen_devices_prev_group(void);
bool ui_screen_devices_next_item(void);
bool ui_screen_devices_prev_item(void);
