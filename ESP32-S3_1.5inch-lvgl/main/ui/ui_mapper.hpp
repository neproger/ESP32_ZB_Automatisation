#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_core/zb_model.h"

typedef struct
{
    bool onoff;
    bool level;
    bool color;
    bool temperature;
    bool humidity;
    bool battery;
    bool occupancy;
} ui_endpoint_caps_t;

void ui_mapper_caps_from_endpoint(const gw_zb_endpoint_t *ep, ui_endpoint_caps_t *out);
bool ui_mapper_supports_key(const ui_endpoint_caps_t *caps, const char *key);

