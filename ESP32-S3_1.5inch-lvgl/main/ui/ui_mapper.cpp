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
} // namespace

void ui_mapper_caps_from_endpoint(const gw_zb_endpoint_t *ep, ui_endpoint_caps_t *out)
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
