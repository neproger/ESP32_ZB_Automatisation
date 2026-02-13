#include "ui_store.hpp"

#include <stdint.h>
#include <string.h>

#include "gw_core/zb_classify.h"

namespace
{
static constexpr size_t UI_STATE_SNAPSHOT_CAP = 256;
static gw_state_item_t s_state_items[UI_STATE_SNAPSHOT_CAP];
static gw_device_t s_devices_snapshot[UI_STORE_DEVICE_CAP];
static gw_zb_endpoint_t s_eps_snapshot[UI_STORE_ENDPOINT_CAP];

size_t find_device_idx(ui_store_t *store, const char *uid)
{
    if (!store || !uid)
    {
        return SIZE_MAX;
    }
    for (size_t i = 0; i < store->device_count; ++i)
    {
        if (strncmp(store->devices[i].uid.uid, uid, sizeof(store->devices[i].uid.uid)) == 0)
        {
            return i;
        }
    }
    return SIZE_MAX;
}

size_t find_endpoint_idx(ui_device_vm_t *dev, uint8_t endpoint)
{
    if (!dev)
    {
        return SIZE_MAX;
    }
    for (size_t i = 0; i < dev->endpoint_count; ++i)
    {
        if (dev->endpoints[i].endpoint_id == endpoint)
        {
            return i;
        }
    }
    return SIZE_MAX;
}

void apply_state_to_endpoint(ui_endpoint_vm_t *ep, const gw_state_item_t *st)
{
    if (!ep || !st)
    {
        return;
    }
    if (strcmp(st->key, "onoff") == 0 && st->value_type == GW_STATE_VALUE_BOOL && ep->caps.onoff)
    {
        ep->has_onoff = true;
        ep->onoff = st->value_bool;
    }
    else if (strcmp(st->key, "level") == 0 && st->value_type == GW_STATE_VALUE_U32 && ep->caps.level)
    {
        ep->has_level = true;
        ep->level = (uint16_t)st->value_u32;
    }
    else if (strcmp(st->key, "temperature_c") == 0 && st->value_type == GW_STATE_VALUE_F32 && ep->caps.temperature)
    {
        ep->has_temperature_c = true;
        ep->temperature_c = st->value_f32;
    }
    else if (strcmp(st->key, "humidity_pct") == 0 && st->value_type == GW_STATE_VALUE_F32 && ep->caps.humidity)
    {
        ep->has_humidity_pct = true;
        ep->humidity_pct = st->value_f32;
    }
    else if (strcmp(st->key, "battery_pct") == 0 && st->value_type == GW_STATE_VALUE_U32 && ep->caps.battery)
    {
        ep->has_battery_pct = true;
        ep->battery_pct = st->value_u32;
    }
    else if (strcmp(st->key, "color_x") == 0 && st->value_type == GW_STATE_VALUE_U32 && ep->caps.color)
    {
        ep->has_color_x = true;
        ep->color_x = (uint16_t)st->value_u32;
    }
    else if (strcmp(st->key, "color_y") == 0 && st->value_type == GW_STATE_VALUE_U32 && ep->caps.color)
    {
        ep->has_color_y = true;
        ep->color_y = (uint16_t)st->value_u32;
    }
    else if (strcmp(st->key, "color_temp_mireds") == 0 && st->value_type == GW_STATE_VALUE_U32 && ep->caps.color)
    {
        ep->has_color_temp_mireds = true;
        ep->color_temp_mireds = (uint16_t)st->value_u32;
    }
}

bool apply_value_from_event(ui_endpoint_vm_t *ep, const gw_event_t *event)
{
    if (!ep || !event)
    {
        return false;
    }
    if ((event->payload_flags & GW_EVENT_PAYLOAD_HAS_CLUSTER) == 0 || (event->payload_flags & GW_EVENT_PAYLOAD_HAS_ATTR) == 0)
    {
        return false;
    }
    if ((event->payload_flags & GW_EVENT_PAYLOAD_HAS_VALUE) == 0)
    {
        return false;
    }

    const uint16_t cluster = event->payload_cluster;
    const uint16_t attr = event->payload_attr;
    const uint8_t value_type = event->payload_value_type;

    if (cluster == 0x0006 && attr == 0x0000 && value_type == GW_EVENT_VALUE_BOOL)
    {
        const bool new_value = (event->payload_value_bool != 0);
        const bool changed = (!ep->has_onoff) || (ep->onoff != new_value);
        ep->has_onoff = true;
        ep->onoff = new_value;
        return changed;
    }
    else if (cluster == 0x0008 && attr == 0x0000)
    {
        if (value_type == GW_EVENT_VALUE_I64)
        {
            const uint16_t new_value = (uint16_t)event->payload_value_i64;
            const bool changed = (!ep->has_level) || (ep->level != new_value);
            ep->has_level = true;
            ep->level = new_value;
            return changed;
        }
    }
    else if (cluster == 0x0402 && attr == 0x0000)
    {
        if (value_type == GW_EVENT_VALUE_F64)
        {
            const float new_value = (float)event->payload_value_f64;
            const bool changed = (!ep->has_temperature_c) || (ep->temperature_c != new_value);
            ep->has_temperature_c = true;
            ep->temperature_c = new_value;
            return changed;
        }
        else if (value_type == GW_EVENT_VALUE_I64)
        {
            const float new_value = ((float)event->payload_value_i64) / 100.0f;
            const bool changed = (!ep->has_temperature_c) || (ep->temperature_c != new_value);
            ep->has_temperature_c = true;
            ep->temperature_c = new_value;
            return changed;
        }
    }
    else if (cluster == 0x0405 && attr == 0x0000)
    {
        if (value_type == GW_EVENT_VALUE_F64)
        {
            const float new_value = (float)event->payload_value_f64;
            const bool changed = (!ep->has_humidity_pct) || (ep->humidity_pct != new_value);
            ep->has_humidity_pct = true;
            ep->humidity_pct = new_value;
            return changed;
        }
        else if (value_type == GW_EVENT_VALUE_I64)
        {
            const float new_value = ((float)event->payload_value_i64) / 100.0f;
            const bool changed = (!ep->has_humidity_pct) || (ep->humidity_pct != new_value);
            ep->has_humidity_pct = true;
            ep->humidity_pct = new_value;
            return changed;
        }
    }
    else if (cluster == 0x0001 && attr == 0x0021 && value_type == GW_EVENT_VALUE_I64)
    {
        const uint32_t new_value = (uint32_t)event->payload_value_i64;
        const bool changed = (!ep->has_battery_pct) || (ep->battery_pct != new_value);
        ep->has_battery_pct = true;
        ep->battery_pct = new_value;
        return changed;
    }
    else if (cluster == 0x0300 && attr == 0x0003 && value_type == GW_EVENT_VALUE_I64)
    {
        const uint16_t new_value = (uint16_t)event->payload_value_i64;
        const bool changed = (!ep->has_color_x) || (ep->color_x != new_value);
        ep->has_color_x = true;
        ep->color_x = new_value;
        return changed;
    }
    else if (cluster == 0x0300 && attr == 0x0004 && value_type == GW_EVENT_VALUE_I64)
    {
        const uint16_t new_value = (uint16_t)event->payload_value_i64;
        const bool changed = (!ep->has_color_y) || (ep->color_y != new_value);
        ep->has_color_y = true;
        ep->color_y = new_value;
        return changed;
    }
    else if (cluster == 0x0300 && attr == 0x0007 && value_type == GW_EVENT_VALUE_I64)
    {
        const uint16_t new_value = (uint16_t)event->payload_value_i64;
        const bool changed = (!ep->has_color_temp_mireds) || (ep->color_temp_mireds != new_value);
        ep->has_color_temp_mireds = true;
        ep->color_temp_mireds = new_value;
        return changed;
    }
    return false;
}

void load_state_for_device(ui_device_vm_t *dev)
{
    if (!dev)
    {
        return;
    }
    size_t count = gw_state_store_list_uid(&dev->uid, s_state_items, UI_STATE_SNAPSHOT_CAP);
    for (size_t i = 0; i < count; ++i)
    {
        for (size_t ep_i = 0; ep_i < dev->endpoint_count; ++ep_i)
        {
            if (s_state_items[i].endpoint != dev->endpoints[ep_i].endpoint_id)
            {
                continue;
            }
            apply_state_to_endpoint(&dev->endpoints[ep_i], &s_state_items[i]);
        }
    }
}
} // namespace

void ui_store_init(ui_store_t *store)
{
    if (!store)
    {
        return;
    }
    memset(store, 0, sizeof(*store));
}

void ui_store_reload(ui_store_t *store)
{
    if (!store)
    {
        return;
    }
    store->device_count = 0;

    const size_t count = gw_device_registry_list(s_devices_snapshot, UI_STORE_DEVICE_CAP);
    for (size_t i = 0; i < count; ++i)
    {
        ui_device_vm_t *dst = &store->devices[store->device_count++];
        memset(dst, 0, sizeof(*dst));
        dst->uid = s_devices_snapshot[i].device_uid;
        dst->short_addr = s_devices_snapshot[i].short_addr;
        strlcpy(dst->name, s_devices_snapshot[i].name, sizeof(dst->name));

        const size_t ep_count = gw_device_registry_list_endpoints(&dst->uid, s_eps_snapshot, UI_STORE_ENDPOINT_CAP);
        dst->endpoint_count = (ep_count > UI_STORE_ENDPOINT_CAP) ? UI_STORE_ENDPOINT_CAP : ep_count;
        for (size_t ep_i = 0; ep_i < dst->endpoint_count; ++ep_i)
        {
            ui_endpoint_vm_t *out_ep = &dst->endpoints[ep_i];
            memset(out_ep, 0, sizeof(*out_ep));
            out_ep->endpoint_id = s_eps_snapshot[ep_i].endpoint;
            strlcpy(out_ep->kind, gw_zb_endpoint_kind(&s_eps_snapshot[ep_i]), sizeof(out_ep->kind));
            ui_mapper_caps_from_endpoint(&s_eps_snapshot[ep_i], &out_ep->caps);
        }

        load_state_for_device(dst);
    }

    if (store->active_device_idx >= store->device_count)
    {
        store->active_device_idx = 0;
    }
}

bool ui_store_apply_event(ui_store_t *store, const gw_event_t *event)
{
    if (!store || !event || !event->type[0])
    {
        return false;
    }

    if (strcmp(event->type, "device.join") == 0 || strcmp(event->type, "device.leave") == 0 ||
        strcmp(event->type, "device.changed") == 0 || strcmp(event->type, "device.update") == 0)
    {
        ui_store_reload(store);
        return true;
    }

    const bool is_state_event =
        (strcmp(event->type, "zigbee.attr_report") == 0) ||
        (strcmp(event->type, "zigbee.attr_read") == 0) ||
        (strcmp(event->type, "zigbee.read_attr") == 0) ||
        (strcmp(event->type, "zigbee.read_attr_resp") == 0 &&
         (event->payload_flags & GW_EVENT_PAYLOAD_HAS_VALUE) &&
         (event->payload_flags & GW_EVENT_PAYLOAD_HAS_ENDPOINT) &&
         (event->payload_flags & GW_EVENT_PAYLOAD_HAS_CLUSTER) &&
         (event->payload_flags & GW_EVENT_PAYLOAD_HAS_ATTR));
    if (!is_state_event)
    {
        return false;
    }
    if ((event->payload_flags & GW_EVENT_PAYLOAD_HAS_ENDPOINT) == 0 || event->device_uid[0] == '\0')
    {
        return false;
    }

    const size_t dev_idx = find_device_idx(store, event->device_uid);
    if (dev_idx == SIZE_MAX)
    {
        return false;
    }
    ui_device_vm_t *dev = &store->devices[dev_idx];
    const size_t ep_idx = find_endpoint_idx(dev, event->payload_endpoint);
    if (ep_idx == SIZE_MAX)
    {
        return false;
    }
    return apply_value_from_event(&dev->endpoints[ep_idx], event);
}

bool ui_store_next_device(ui_store_t *store)
{
    if (!store || store->device_count == 0)
    {
        return false;
    }
    store->active_device_idx = (store->active_device_idx + 1) % store->device_count;
    return true;
}

bool ui_store_prev_device(ui_store_t *store)
{
    if (!store || store->device_count == 0)
    {
        return false;
    }
    if (store->active_device_idx == 0)
    {
        store->active_device_idx = store->device_count - 1;
    }
    else
    {
        store->active_device_idx--;
    }
    return true;
}

const ui_device_vm_t *ui_store_active_device(const ui_store_t *store)
{
    if (!store || store->device_count == 0 || store->active_device_idx >= store->device_count)
    {
        return nullptr;
    }
    return &store->devices[store->active_device_idx];
}
