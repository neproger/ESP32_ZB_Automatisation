#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gw_proto/gw_proto.h"
#include "gw_model/gw_model_groups.h"
#include "widget_endpoint_ref.hpp"

struct WidgetGroupItemState {
    WidgetEndpointRef ref = {};
    char label[32] = {0};
    uint32_t version = 0;
};

struct WidgetGroupState {
    bool has_group = false;
    gw_proto_group_v1_t group = {};
    size_t group_count = 0;
    size_t active_group_index = 0;
    size_t item_count = 0;
    size_t active_item_index = 0;
    bool has_active_item = false;
    WidgetGroupItemState active_item = {};
};

bool widget_group_state_read(size_t group_index, size_t item_index, WidgetGroupState *out_state);
