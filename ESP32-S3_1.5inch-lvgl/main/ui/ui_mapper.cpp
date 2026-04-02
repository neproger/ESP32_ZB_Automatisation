#include "ui_mapper.hpp"

#include <string.h>

namespace
{
bool has_cluster(const uint16_t *clusters, uint8_t count, uint16_t id)
{
    if (!clusters)
    {
        return false;
    }
    for (uint8_t i = 0; i < count; ++i)
    {
        if (clusters[i] == id)
        {
            return true;
        }
    }
    return false;
}

const char *kind_from_standard_device_type(uint16_t profile_id, uint16_t device_id)
{
    if (profile_id != 0x0104) {
        return nullptr;
    }

    switch (device_id) {
        case 0x0100:
        case 0x0108:
            return "relay";
        case 0x0101:
            return "dimmable_light";
        case 0x0102:
            return "color_light";
        case 0x0103:
            return "switch";
        case 0x0104:
        case 0x0105:
            return "dimmer_switch";
        case 0x0107:
            return "occupancy_sensor";
        case 0x010D:
            return "temperature_sensor";
        case 0x010E:
            return "illuminance_sensor";
        default:
            return nullptr;
    }
}

bool out_clusters_fit_button_pattern(const uint16_t *clusters, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i) {
        const uint16_t cluster = clusters[i];
        if (cluster != 0x000A && cluster != 0x0019) {
            return false;
        }
    }
    return true;
}
} // namespace

void ui_mapper_caps_from_proto_endpoint(const gw_proto_endpoint_v1_t *ep, ui_endpoint_caps_t *out)
{
    if (!ep || !out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->onoff = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0006);
    out->level = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0008);
    out->color = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0300);
    out->temperature = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0402);
    out->humidity = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0405);
    out->battery = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0001);
    out->occupancy = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0406);
}

const char *ui_mapper_kind_from_proto_endpoint(const gw_proto_endpoint_v1_t *ep)
{
    if (!ep) {
        return "unknown";
    }

    const char *standard_kind = kind_from_standard_device_type(ep->profile_id, ep->device_id);
    if (standard_kind) {
        return standard_kind;
    }

    const bool onoff_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0006);
    const bool onoff_cli = has_cluster(ep->out_clusters, ep->out_cluster_count, 0x0006);
    const bool level_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0008);
    const bool color_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0300);
    const bool power_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0001);
    const bool temp_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0402);
    const bool hum_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0405);
    const bool occ_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0406);
    const bool illum_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0400);
    const bool press_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0403);
    const bool flow_srv = has_cluster(ep->in_clusters, ep->in_cluster_count, 0x0404);

    const bool button_like = onoff_srv && power_srv && !level_srv && !color_srv && !onoff_cli &&
                             out_clusters_fit_button_pattern(ep->out_clusters, ep->out_cluster_count);

    if (color_srv) {
        return "color_light";
    }
    if (level_srv && onoff_srv) {
        return "dimmable_light";
    }
    if (button_like) {
        return "switch";
    }
    if (onoff_srv) {
        return "relay";
    }
    if (onoff_cli) {
        if (has_cluster(ep->out_clusters, ep->out_cluster_count, 0x0008)) {
            return "dimmer_switch";
        }
        return "switch";
    }
    if (temp_srv || hum_srv || occ_srv || illum_srv || press_srv || flow_srv) {
        if (temp_srv && hum_srv) return "temp_humidity_sensor";
        if (temp_srv) return "temperature_sensor";
        if (hum_srv) return "humidity_sensor";
        if (occ_srv) return "occupancy_sensor";
        if (illum_srv) return "illuminance_sensor";
        if (press_srv) return "pressure_sensor";
        if (flow_srv) return "flow_sensor";
        return "sensor";
    }

    return "unknown";
}

bool ui_mapper_supports_key(const ui_endpoint_caps_t *caps, const char *key)
{
    if (!caps || !key || !key[0])
    {
        return false;
    }
    if (strcmp(key, "onoff") == 0)
    {
        return caps->onoff;
    }
    if (strcmp(key, "level") == 0)
    {
        return caps->level;
    }
    if (strcmp(key, "temperature_c") == 0)
    {
        return caps->temperature;
    }
    if (strcmp(key, "humidity_pct") == 0)
    {
        return caps->humidity;
    }
    if (strcmp(key, "battery_pct") == 0)
    {
        return caps->battery;
    }
    if (strcmp(key, "occupancy") == 0)
    {
        return caps->occupancy;
    }
    if (strcmp(key, "color_x") == 0 || strcmp(key, "color_y") == 0)
    {
        return caps->color;
    }
    return false;
}
