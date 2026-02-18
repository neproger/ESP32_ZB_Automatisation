#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "ui_store.hpp"

typedef enum
{
    UI_WIDGET_VALUE_NONE = 0,
    UI_WIDGET_VALUE_BOOL = 1,
    UI_WIDGET_VALUE_U32 = 2,
    UI_WIDGET_VALUE_F32 = 3,
} ui_widget_value_type_t;

typedef struct
{
    ui_widget_value_type_t type;
    bool has_value;
    union
    {
        bool b;
        uint32_t u32;
        float f32;
    } v;
} ui_widget_value_t;

void ui_widgets_reset(void);
lv_obj_t *ui_widgets_create_endpoint_card(lv_obj_t *parent, const gw_device_uid_t *uid, const ui_endpoint_vm_t *ep);
bool ui_widgets_set_state(const char *device_uid, uint8_t endpoint, const char *key, const ui_widget_value_t *value);
