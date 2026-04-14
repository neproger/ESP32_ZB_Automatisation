#pragma once

#include "lvgl.h"

void ui_screen_devices_init(lv_obj_t *root);
void ui_screen_devices_render_if_needed(void);
void ui_screen_devices_step_group(int delta);
void ui_screen_devices_step_item(int delta);
void ui_screen_devices_clear_pending_navigation(void);
