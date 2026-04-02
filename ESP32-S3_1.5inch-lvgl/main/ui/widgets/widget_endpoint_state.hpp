#pragma once

#include <stdint.h>

#include "gw_core/gw_proto.h"
#include "gw_model/gw_model_state.h"
#include "gw_model/gw_model_topology.h"
#include "ui_mapper.hpp"
#include "widget_endpoint_ref.hpp"

struct WidgetEndpointState {
    WidgetEndpointRef ref = {};

    bool has_device = false;
    gw_proto_device_v1_t device = {};

    bool has_endpoint = false;
    gw_proto_endpoint_v1_t endpoint = {};
    ui_endpoint_caps_t caps = {};

    uint32_t topology_version = 0;
    uint32_t state_version = 0;

    bool has_onoff = false;
    bool onoff = false;
    uint32_t onoff_version = 0;

    bool has_level = false;
    uint32_t level = 0;
    uint32_t level_version = 0;

    bool has_temperature_c = false;
    float temperature_c = 0.0f;
    uint32_t temperature_c_version = 0;

    bool has_humidity_pct = false;
    float humidity_pct = 0.0f;
    uint32_t humidity_pct_version = 0;

    bool has_battery_pct = false;
    uint32_t battery_pct = 0;
    uint32_t battery_pct_version = 0;

    bool has_color_x = false;
    uint32_t color_x = 0;
    uint32_t color_x_version = 0;

    bool has_color_y = false;
    uint32_t color_y = 0;
    uint32_t color_y_version = 0;
};

bool widget_endpoint_state_read(const WidgetEndpointRef &ref, WidgetEndpointState *out_state);
