#pragma once

#ifdef __cplusplus
/* C++ UI API */

/* Initialize minimal application UI (LVGL v9) */
void ui_app_init(void);

extern "C"
{
#endif

    /* Input hooks used by device layer callbacks */
    void LVGL_knob_event(void *event);
    void LVGL_button_event(void *event);

#ifdef __cplusplus
}
#endif
