#pragma once

#include "lvgl.h"

#ifdef __cplusplus
/* C++ UI API */

/* Initialize minimal application UI (LVGL v9) */
void ui_app_init(void);
void ui_app_attach_screen_input(lv_obj_t *screen);

extern "C"
{
#endif

    /* Input hooks used by device layer callbacks */
    void ui_post_knob_event(void *event);
    void ui_post_button_event(void *event);

#ifdef __cplusplus
}
#endif
