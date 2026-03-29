#pragma once

#include <stdint.h>

#include "gw_core/device_registry.h"
#include "gw_core/state_store.h"
#include "ui_mapper.hpp"
#include "widget_endpoint_ref.hpp"

struct WidgetEndpointState {
    WidgetEndpointRef ref = {};

    bool has_device = false;
    gw_device_t device = {};

    bool has_endpoint = false;
    gw_zb_endpoint_t endpoint = {};
    ui_endpoint_caps_t caps = {};

    uint32_t topology_version = 0;
    uint32_t state_version = 0;

    bool has_onoff = false;
    bool onoff = false;

    bool has_level = false;
    uint32_t level = 0;

    bool has_temperature_c = false;
    float temperature_c = 0.0f;

    bool has_humidity_pct = false;
    float humidity_pct = 0.0f;

    bool has_battery_pct = false;
    uint32_t battery_pct = 0;

    bool has_color_x = false;
    uint32_t color_x = 0;

    bool has_color_y = false;
    uint32_t color_y = 0;
};

bool widget_endpoint_state_read(const WidgetEndpointRef &ref, WidgetEndpointState *out_state);
