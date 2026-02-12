#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "gw_core/event_bus.h"

esp_err_t ui_events_bridge_init(void);
size_t ui_events_bridge_drain(gw_event_t *out, size_t max_out);

