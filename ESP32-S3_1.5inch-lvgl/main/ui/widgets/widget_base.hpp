#pragma once

#include "lvgl.h"

class WidgetBase {
public:
    virtual ~WidgetBase() = default;

    virtual lv_obj_t *root() const = 0;
    virtual void render() = 0;
    virtual void render_if_needed() = 0;
};
