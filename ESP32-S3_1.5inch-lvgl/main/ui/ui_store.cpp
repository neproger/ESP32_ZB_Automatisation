#include "ui_store.hpp"

#include <stdint.h>
#include <string.h>

#include "gw_core/zb_classify.h"

namespace
{
static constexpr size_t UI_STATE_SNAPSHOT_CAP = 256;
static constexpr size_t UI_GROUP_ITEM_SNAPSHOT_CAP = 256;

static gw_state_item_t s_state_items[UI_STATE_SNAPSHOT_CAP];
static gw_device_t s_devices_snapshot[UI_STORE_DEVICE_CAP];
static gw_zb_endpoint_t s_eps_snapshot[UI_STORE_ENDPOINT_CAP];
static gw_group_entry_t s_groups_snapshot[UI_STORE_GROUP_CAP];
static gw_group_item_t s_group_items_snapshot[UI_GROUP_ITEM_SNAPSHOT_CAP];

size_t find_group_idx(ui_store_t *store, const char *group_id)
{
    if (!store || !group_id) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i < store->group_count; ++i) {
        if (strncmp(store->groups[i].id, group_id, sizeof(store->groups[i].id)) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

const gw_device_t *find_device_snapshot(const gw_device_uid_t *uid, size_t count)
{
    if (!uid) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        if (strncmp(s_devices_snapshot[i].device_uid.uid, uid->uid, sizeof(uid->uid)) == 0) {
            return &s_devices_snapshot[i];
        }
    }
    return nullptr;
}

bool load_endpoint_snapshot(const gw_device_uid_t *uid, uint8_t endpoint, gw_zb_endpoint_t *out_ep)
{
    if (!uid || !out_ep || endpoint == 0) {
        return false;
    }
    const size_t ep_count = gw_device_registry_list_endpoints(uid, s_eps_snapshot, UI_STORE_ENDPOINT_CAP);
    for (size_t i = 0; i < ep_count; ++i) {
        if (s_eps_snapshot[i].endpoint == endpoint) {
            *out_ep = s_eps_snapshot[i];
            return true;
        }
    }
    return false;
}

void apply_state_to_endpoint(ui_endpoint_vm_t *ep, const gw_state_item_t *st)
{
    if (!ep || !st) {
        return;
    }
    if (strcmp(st->key, "onoff") == 0 && st->value_type == GW_STATE_VALUE_BOOL && ep->caps.onoff) {
        ep->has_onoff = true;
        ep->onoff = st->value_bool;
    } else if (strcmp(st->key, "level") == 0 && st->value_type == GW_STATE_VALUE_U32 && ep->caps.level) {
        ep->has_level = true;
        ep->level = (uint16_t)st->value_u32;
    } else if (strcmp(st->key, "temperature_c") == 0 && st->value_type == GW_STATE_VALUE_F32 && ep->caps.temperature) {
        ep->has_temperature_c = true;
        ep->temperature_c = st->value_f32;
    } else if (strcmp(st->key, "humidity_pct") == 0 && st->value_type == GW_STATE_VALUE_F32 && ep->caps.humidity) {
        ep->has_humidity_pct = true;
        ep->humidity_pct = st->value_f32;
    } else if (strcmp(st->key, "battery_pct") == 0 && st->value_type == GW_STATE_VALUE_U32 && ep->caps.battery) {
        ep->has_battery_pct = true;
        ep->battery_pct = st->value_u32;
    } else if (strcmp(st->key, "color_x") == 0 && st->value_type == GW_STATE_VALUE_U32 && ep->caps.color) {
        ep->has_color_x = true;
        ep->color_x = (uint16_t)st->value_u32;
    } else if (strcmp(st->key, "color_y") == 0 && st->value_type == GW_STATE_VALUE_U32 && ep->caps.color) {
        ep->has_color_y = true;
        ep->color_y = (uint16_t)st->value_u32;
    }
}

void load_state_for_item(ui_group_item_vm_t *item)
{
    if (!item) {
        return;
    }
    const size_t count = gw_state_store_list_uid(&item->uid, s_state_items, UI_STATE_SNAPSHOT_CAP);
    for (size_t i = 0; i < count; ++i) {
        if (s_state_items[i].endpoint != item->endpoint_id) {
            continue;
        }
        apply_state_to_endpoint(&item->endpoint, &s_state_items[i]);
    }
}

bool apply_value_from_event(ui_endpoint_vm_t *ep, const gw_event_t *event)
{
    if (!ep || !event) {
        return false;
    }
    if ((event->payload_flags & GW_EVENT_PAYLOAD_HAS_CLUSTER) == 0 ||
        (event->payload_flags & GW_EVENT_PAYLOAD_HAS_ATTR) == 0 ||
        (event->payload_flags & GW_EVENT_PAYLOAD_HAS_VALUE) == 0) {
        return false;
    }

    const uint16_t cluster = event->payload_cluster;
    const uint16_t attr = event->payload_attr;
    const uint8_t value_type = event->payload_value_type;

    if (cluster == 0x0006 && attr == 0x0000 && value_type == GW_EVENT_VALUE_BOOL) {
        const bool v = (event->payload_value_bool != 0);
        const bool changed = (!ep->has_onoff) || (ep->onoff != v);
        ep->has_onoff = true;
        ep->onoff = v;
        return changed;
    }
    if (cluster == 0x0008 && attr == 0x0000 && value_type == GW_EVENT_VALUE_I64) {
        const uint16_t v = (uint16_t)event->payload_value_i64;
        const bool changed = (!ep->has_level) || (ep->level != v);
        ep->has_level = true;
        ep->level = v;
        return changed;
    }
    if (cluster == 0x0402 && attr == 0x0000) {
        float v = 0.0f;
        if (value_type == GW_EVENT_VALUE_F64) v = (float)event->payload_value_f64;
        else if (value_type == GW_EVENT_VALUE_I64) v = ((float)event->payload_value_i64) / 100.0f;
        else return false;
        const bool changed = (!ep->has_temperature_c) || (ep->temperature_c != v);
        ep->has_temperature_c = true;
        ep->temperature_c = v;
        return changed;
    }
    if (cluster == 0x0405 && attr == 0x0000) {
        float v = 0.0f;
        if (value_type == GW_EVENT_VALUE_F64) v = (float)event->payload_value_f64;
        else if (value_type == GW_EVENT_VALUE_I64) v = ((float)event->payload_value_i64) / 100.0f;
        else return false;
        const bool changed = (!ep->has_humidity_pct) || (ep->humidity_pct != v);
        ep->has_humidity_pct = true;
        ep->humidity_pct = v;
        return changed;
    }
    if (cluster == 0x0001 && attr == 0x0021 && value_type == GW_EVENT_VALUE_I64) {
        const uint32_t v = (uint32_t)event->payload_value_i64;
        const bool changed = (!ep->has_battery_pct) || (ep->battery_pct != v);
        ep->has_battery_pct = true;
        ep->battery_pct = v;
        return changed;
    }
    if (cluster == 0x0300 && attr == 0x0003 && value_type == GW_EVENT_VALUE_I64) {
        const uint16_t v = (uint16_t)event->payload_value_i64;
        const bool changed = (!ep->has_color_x) || (ep->color_x != v);
        ep->has_color_x = true;
        ep->color_x = v;
        return changed;
    }
    if (cluster == 0x0300 && attr == 0x0004 && value_type == GW_EVENT_VALUE_I64) {
        const uint16_t v = (uint16_t)event->payload_value_i64;
        const bool changed = (!ep->has_color_y) || (ep->color_y != v);
        ep->has_color_y = true;
        ep->color_y = v;
        return changed;
    }
    return false;
}

void sort_group_items_by_order(ui_group_vm_t *group)
{
    if (!group || group->item_count < 2) {
        return;
    }
    for (size_t i = 0; i + 1 < group->item_count; ++i) {
        for (size_t j = i + 1; j < group->item_count; ++j) {
            if (group->items[j].order < group->items[i].order) {
                ui_group_item_vm_t tmp = group->items[i];
                group->items[i] = group->items[j];
                group->items[j] = tmp;
            }
        }
    }
}

} // namespace

void ui_store_init(ui_store_t *store)
{
    if (!store) {
        return;
    }
    memset(store, 0, sizeof(*store));
}

void ui_store_reload(ui_store_t *store)
{
    if (!store) {
        return;
    }
    store->group_count = 0;

    const size_t dev_count = gw_device_registry_list(s_devices_snapshot, UI_STORE_DEVICE_CAP);
    const size_t group_count = gw_group_store_list(s_groups_snapshot, UI_STORE_GROUP_CAP);

    for (size_t i = 0; i < group_count && i < UI_STORE_GROUP_CAP; ++i) {
        ui_group_vm_t *g = &store->groups[store->group_count++];
        memset(g, 0, sizeof(*g));
        strlcpy(g->id, s_groups_snapshot[i].id, sizeof(g->id));
        strlcpy(g->name, s_groups_snapshot[i].name, sizeof(g->name));
    }

    const size_t item_count = gw_group_store_list_items(s_group_items_snapshot, UI_GROUP_ITEM_SNAPSHOT_CAP);
    for (size_t i = 0; i < item_count; ++i) {
        const gw_group_item_t *src = &s_group_items_snapshot[i];
        const size_t gidx = find_group_idx(store, src->group_id);
        if (gidx == SIZE_MAX) {
            continue;
        }
        ui_group_vm_t *group = &store->groups[gidx];
        if (group->item_count >= UI_STORE_GROUP_ITEM_CAP) {
            continue;
        }

        gw_zb_endpoint_t ep_snap = {};
        if (!load_endpoint_snapshot(&src->device_uid, src->endpoint, &ep_snap)) {
            continue;
        }

        ui_group_item_vm_t *dst = &group->items[group->item_count++];
        memset(dst, 0, sizeof(*dst));
        dst->uid = src->device_uid;
        dst->endpoint_id = src->endpoint;
        dst->order = src->order;
        strlcpy(dst->label, src->label, sizeof(dst->label));

        const gw_device_t *dev = find_device_snapshot(&src->device_uid, dev_count);
        if (dev) {
            dst->short_addr = dev->short_addr;
            strlcpy(dst->device_name, dev->name, sizeof(dst->device_name));
        } else {
            strlcpy(dst->device_name, src->device_uid.uid, sizeof(dst->device_name));
        }

        dst->endpoint.endpoint_id = src->endpoint;
        strlcpy(dst->endpoint.kind, gw_zb_endpoint_kind(&ep_snap), sizeof(dst->endpoint.kind));
        ui_mapper_caps_from_endpoint(&ep_snap, &dst->endpoint.caps);
        load_state_for_item(dst);
    }

    for (size_t i = 0; i < store->group_count; ++i) {
        sort_group_items_by_order(&store->groups[i]);
        if (store->groups[i].active_item_idx >= store->groups[i].item_count) {
            store->groups[i].active_item_idx = 0;
        }
    }
    if (store->active_group_idx >= store->group_count) {
        store->active_group_idx = 0;
    }
}

bool ui_store_apply_event(ui_store_t *store, const gw_event_t *event)
{
    if (!store || !event || !event->type[0]) {
        return false;
    }

    const bool structural =
        (strcmp(event->type, "device.join") == 0) ||
        (strcmp(event->type, "device.leave") == 0) ||
        (strcmp(event->type, "device.changed") == 0) ||
        (strcmp(event->type, "device.update") == 0) ||
        (strcmp(event->type, "group.changed") == 0);
    if (structural) {
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
    if (!is_state_event || event->device_uid[0] == '\0' ||
        (event->payload_flags & GW_EVENT_PAYLOAD_HAS_ENDPOINT) == 0) {
        return false;
    }

    bool changed = false;
    for (size_t gi = 0; gi < store->group_count; ++gi) {
        ui_group_vm_t *group = &store->groups[gi];
        for (size_t ii = 0; ii < group->item_count; ++ii) {
            ui_group_item_vm_t *item = &group->items[ii];
            if (item->endpoint_id != event->payload_endpoint) {
                continue;
            }
            if (strncmp(item->uid.uid, event->device_uid, sizeof(item->uid.uid)) != 0) {
                continue;
            }
            if (apply_value_from_event(&item->endpoint, event)) {
                changed = true;
            }
        }
    }
    return changed;
}

bool ui_store_next_group(ui_store_t *store)
{
    if (!store || store->group_count == 0) {
        return false;
    }
    store->active_group_idx = (store->active_group_idx + 1) % store->group_count;
    return true;
}

bool ui_store_prev_group(ui_store_t *store)
{
    if (!store || store->group_count == 0) {
        return false;
    }
    if (store->active_group_idx == 0) {
        store->active_group_idx = store->group_count - 1;
    } else {
        store->active_group_idx--;
    }
    return true;
}

bool ui_store_next_item(ui_store_t *store)
{
    if (!store || store->group_count == 0 || store->active_group_idx >= store->group_count) {
        return false;
    }
    ui_group_vm_t *group = &store->groups[store->active_group_idx];
    if (group->item_count <= 1) {
        return false;
    }
    group->active_item_idx = (group->active_item_idx + 1) % group->item_count;
    return true;
}

bool ui_store_prev_item(ui_store_t *store)
{
    if (!store || store->group_count == 0 || store->active_group_idx >= store->group_count) {
        return false;
    }
    ui_group_vm_t *group = &store->groups[store->active_group_idx];
    if (group->item_count <= 1) {
        return false;
    }
    if (group->active_item_idx == 0) {
        group->active_item_idx = group->item_count - 1;
    } else {
        group->active_item_idx--;
    }
    return true;
}

const ui_group_vm_t *ui_store_active_group(const ui_store_t *store)
{
    if (!store || store->group_count == 0 || store->active_group_idx >= store->group_count) {
        return nullptr;
    }
    return &store->groups[store->active_group_idx];
}

const ui_group_item_vm_t *ui_store_active_item(const ui_store_t *store)
{
    const ui_group_vm_t *group = ui_store_active_group(store);
    if (!group || group->item_count == 0 || group->active_item_idx >= group->item_count) {
        return nullptr;
    }
    return &group->items[group->active_item_idx];
}
