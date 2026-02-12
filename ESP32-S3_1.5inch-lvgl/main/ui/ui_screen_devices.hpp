#pragma once

#include "lvgl.h"

#include "ui_store.hpp"

void ui_screen_devices_init(lv_obj_t *root);
void ui_screen_devices_render(const ui_store_t *store);
