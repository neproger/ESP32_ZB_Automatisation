#pragma once

#include "esp_err.h"

typedef enum {
    BUTTON_EVENT_SHORT_PRESS = 0,
    BUTTON_EVENT_LONG_PRESS,
} button_event_t;

typedef void (*button_event_cb_t)(button_event_t event, void *user_ctx);

/* Initialize the on-board BOOT button (GPIO9) with a polling task. */
esp_err_t button_init(button_event_cb_t cb, void *user_ctx);
