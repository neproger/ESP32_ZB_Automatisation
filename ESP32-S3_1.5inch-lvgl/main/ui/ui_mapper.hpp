#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_proto/gw_proto.h"

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

void ui_mapper_caps_from_proto_endpoint(const gw_proto_endpoint_v1_t *ep, ui_endpoint_caps_t *out);
const char *ui_mapper_kind_from_proto_endpoint(const gw_proto_endpoint_v1_t *ep);
bool ui_mapper_supports_key(const ui_endpoint_caps_t *caps, const char *key);
