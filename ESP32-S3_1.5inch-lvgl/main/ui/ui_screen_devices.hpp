#pragma once

#include "lvgl.h"

#include "ui_store.hpp"

void ui_screen_devices_init(lv_obj_t *root);
void ui_screen_devices_render(const ui_store_t *store);
void ui_screen_devices_apply_state_event(const ui_store_t *store, const gw_event_t *event);
