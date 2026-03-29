#include "widget_endpoint_state.hpp"

#include <string.h>

#include "esp_attr.h"

namespace
{
EXT_RAM_BSS_ATTR static gw_zb_endpoint_t s_endpoints_snapshot[GW_ZB_MAX_ENDPOINTS];

void update_version(uint32_t *current, uint32_t value)
{
    if (!current) {
        return;
    }
    if (value > *current) {
        *current = value;
    }
}

void read_bool_key(const WidgetEndpointRef &ref,
                   const char *key,
                   bool *out_has_value,
                   bool *out_value,
                   uint32_t *out_version)
{
    if (!key || !out_has_value || !out_value || !out_version) {
        return;
    }
    gw_state_item_t item = {};
    if (gw_state_store_get(&ref.uid, ref.endpoint, key, &item) != ESP_OK) {
        *out_has_value = false;
        return;
    }
    if (item.value_type != GW_STATE_VALUE_BOOL) {
        *out_has_value = false;
        return;
    }
    *out_has_value = true;
    *out_value = item.value_bool;
    update_version(out_version, item.version);
}

void read_u32_key(const WidgetEndpointRef &ref,
                  const char *key,
                  bool *out_has_value,
                  uint32_t *out_value,
                  uint32_t *out_version)
{
    if (!key || !out_has_value || !out_value || !out_version) {
        return;
    }
    gw_state_item_t item = {};
    if (gw_state_store_get(&ref.uid, ref.endpoint, key, &item) != ESP_OK) {
        *out_has_value = false;
        return;
    }
    if (item.value_type != GW_STATE_VALUE_U32) {
        *out_has_value = false;
        return;
    }
    *out_has_value = true;
    *out_value = item.value_u32;
    update_version(out_version, item.version);
}

void read_f32_key(const WidgetEndpointRef &ref,
                  const char *key,
                  bool *out_has_value,
                  float *out_value,
                  uint32_t *out_version)
{
    if (!key || !out_has_value || !out_value || !out_version) {
        return;
    }
    gw_state_item_t item = {};
    if (gw_state_store_get(&ref.uid, ref.endpoint, key, &item) != ESP_OK) {
        *out_has_value = false;
        return;
    }
    if (item.value_type != GW_STATE_VALUE_F32) {
        *out_has_value = false;
        return;
    }
    *out_has_value = true;
    *out_value = item.value_f32;
    update_version(out_version, item.version);
}
} // namespace

bool widget_endpoint_state_read(const WidgetEndpointRef &ref, WidgetEndpointState *out_state)
{
    if (!out_state) {
        return false;
    }

    *out_state = WidgetEndpointState{};
    out_state->ref = ref;
    if (!ref.valid()) {
        return false;
    }

    if (gw_device_registry_get(&ref.uid, &out_state->device) == ESP_OK) {
        out_state->has_device = true;
        update_version(&out_state->topology_version, out_state->device.version);
    }

    const size_t endpoint_count = gw_device_registry_list_endpoints(&ref.uid, s_endpoints_snapshot, GW_ZB_MAX_ENDPOINTS);
    for (size_t i = 0; i < endpoint_count; ++i) {
        if (s_endpoints_snapshot[i].endpoint != ref.endpoint) {
            continue;
        }
        out_state->endpoint = s_endpoints_snapshot[i];
        out_state->has_endpoint = true;
        ui_mapper_caps_from_endpoint(&out_state->endpoint, &out_state->caps);
        update_version(&out_state->topology_version, out_state->endpoint.version);
        break;
    }

    if (!out_state->has_endpoint) {
        return out_state->has_device;
    }

    if (out_state->caps.onoff) {
        read_bool_key(ref, "onoff", &out_state->has_onoff, &out_state->onoff, &out_state->state_version);
    }
    if (out_state->caps.level) {
        read_u32_key(ref, "level", &out_state->has_level, &out_state->level, &out_state->state_version);
    }
    if (out_state->caps.temperature) {
        read_f32_key(ref, "temperature_c", &out_state->has_temperature_c, &out_state->temperature_c, &out_state->state_version);
    }
    if (out_state->caps.humidity) {
        read_f32_key(ref, "humidity_pct", &out_state->has_humidity_pct, &out_state->humidity_pct, &out_state->state_version);
    }
    if (out_state->caps.battery) {
        read_u32_key(ref, "battery_pct", &out_state->has_battery_pct, &out_state->battery_pct, &out_state->state_version);
    }
    if (out_state->caps.color) {
        read_u32_key(ref, "color_x", &out_state->has_color_x, &out_state->color_x, &out_state->state_version);
        read_u32_key(ref, "color_y", &out_state->has_color_y, &out_state->color_y, &out_state->state_version);
    }

    return true;
}
