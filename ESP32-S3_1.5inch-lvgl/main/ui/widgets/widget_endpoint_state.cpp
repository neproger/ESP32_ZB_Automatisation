#include "widget_endpoint_state.hpp"

#include <string.h>

#include "esp_attr.h"

namespace
{
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
                   uint32_t *out_value_version,
                   uint32_t *out_state_version)
{
    if (!key || !out_has_value || !out_value || !out_value_version || !out_state_version) {
        return;
    }
    gw_model_state_key_t state_key = {};
    state_key.uid = ref.uid;
    state_key.endpoint = ref.endpoint;
    strlcpy(state_key.key, key, sizeof(state_key.key));

    gw_proto_state_item_v1_t item = {};
    if (gw_model_get_state(&state_key, &item) != ESP_OK) {
        *out_has_value = false;
        *out_value_version = 0;
        return;
    }
    if (item.value_type != GW_STATE_VALUE_BOOL) {
        *out_has_value = false;
        *out_value_version = 0;
        return;
    }
    *out_has_value = true;
    *out_value = item.value_bool;
    *out_value_version = item.version;
    update_version(out_state_version, item.version);
}

void read_u32_key(const WidgetEndpointRef &ref,
                  const char *key,
                  bool *out_has_value,
                  uint32_t *out_value,
                  uint32_t *out_value_version,
                  uint32_t *out_state_version)
{
    if (!key || !out_has_value || !out_value || !out_value_version || !out_state_version) {
        return;
    }
    gw_model_state_key_t state_key = {};
    state_key.uid = ref.uid;
    state_key.endpoint = ref.endpoint;
    strlcpy(state_key.key, key, sizeof(state_key.key));

    gw_proto_state_item_v1_t item = {};
    if (gw_model_get_state(&state_key, &item) != ESP_OK) {
        *out_has_value = false;
        *out_value_version = 0;
        return;
    }
    if (item.value_type != GW_STATE_VALUE_U32) {
        *out_has_value = false;
        *out_value_version = 0;
        return;
    }
    *out_has_value = true;
    *out_value = item.value_u32;
    *out_value_version = item.version;
    update_version(out_state_version, item.version);
}

void read_f32_key(const WidgetEndpointRef &ref,
                  const char *key,
                  bool *out_has_value,
                  float *out_value,
                  uint32_t *out_value_version,
                  uint32_t *out_state_version)
{
    if (!key || !out_has_value || !out_value || !out_value_version || !out_state_version) {
        return;
    }
    gw_model_state_key_t state_key = {};
    state_key.uid = ref.uid;
    state_key.endpoint = ref.endpoint;
    strlcpy(state_key.key, key, sizeof(state_key.key));

    gw_proto_state_item_v1_t item = {};
    if (gw_model_get_state(&state_key, &item) != ESP_OK) {
        *out_has_value = false;
        *out_value_version = 0;
        return;
    }
    if (item.value_type != GW_STATE_VALUE_F32) {
        *out_has_value = false;
        *out_value_version = 0;
        return;
    }
    *out_has_value = true;
    *out_value = item.value_f32;
    *out_value_version = item.version;
    update_version(out_state_version, item.version);
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

    if (gw_model_get_device(&ref.uid, &out_state->device) == ESP_OK) {
        out_state->has_device = true;
        update_version(&out_state->topology_version, out_state->device.version);
    }

    gw_model_endpoint_key_t endpoint_key = {
        .uid = ref.uid,
        .endpoint = ref.endpoint,
    };
    if (gw_model_get_endpoint(&endpoint_key, &out_state->endpoint) == ESP_OK) {
        out_state->has_endpoint = true;
        ui_mapper_caps_from_proto_endpoint(&out_state->endpoint, &out_state->caps);
        update_version(&out_state->topology_version, out_state->endpoint.version);
    }

    if (!out_state->has_endpoint) {
        return out_state->has_device;
    }

    if (out_state->caps.onoff) {
        read_bool_key(ref, "onoff", &out_state->has_onoff, &out_state->onoff, &out_state->onoff_version, &out_state->state_version);
    }
    if (out_state->caps.level) {
        read_u32_key(ref, "level", &out_state->has_level, &out_state->level, &out_state->level_version, &out_state->state_version);
    }
    if (out_state->caps.temperature) {
        read_f32_key(ref, "temperature_c", &out_state->has_temperature_c, &out_state->temperature_c, &out_state->temperature_c_version, &out_state->state_version);
    }
    if (out_state->caps.humidity) {
        read_f32_key(ref, "humidity_pct", &out_state->has_humidity_pct, &out_state->humidity_pct, &out_state->humidity_pct_version, &out_state->state_version);
    }
    if (out_state->caps.battery) {
        read_u32_key(ref, "battery_pct", &out_state->has_battery_pct, &out_state->battery_pct, &out_state->battery_pct_version, &out_state->state_version);
    }
    if (out_state->caps.color) {
        read_u32_key(ref, "color_x", &out_state->has_color_x, &out_state->color_x, &out_state->color_x_version, &out_state->state_version);
        read_u32_key(ref, "color_y", &out_state->has_color_y, &out_state->color_y, &out_state->color_y_version, &out_state->state_version);
    }

    return true;
}
