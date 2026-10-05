#pragma once

/*
 * Совместимость LVGL 9.5 ↔ 9.6. Превью (LVGL Live Preview) идёт на 9.5, прошивка — на
 * 9.6: в 9.6 `lv_obj_clear_flag` объявлен deprecated в пользу адресных сеттеров.
 * Один и тот же ui/ должен собираться на обеих версиях без предупреждений.
 */

#include "lvgl.h"

static inline void ui_set_scrollable(lv_obj_t *obj, bool enabled)
{
#if (LVGL_VERSION_MAJOR == 9) && (LVGL_VERSION_MINOR >= 6)
    lv_obj_set_scrollable(obj, enabled);
#else
    if (enabled) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
#endif
}

static inline void ui_set_hidden(lv_obj_t *obj, bool hidden)
{
#if (LVGL_VERSION_MAJOR == 9) && (LVGL_VERSION_MINOR >= 6)
    lv_obj_set_hidden(obj, hidden);
#else
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}
