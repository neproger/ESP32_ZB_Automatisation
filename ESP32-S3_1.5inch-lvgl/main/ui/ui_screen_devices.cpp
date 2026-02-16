#include "ui_screen_devices.hpp"

#include <string.h>

#include "gw_core/types.h"
#include "ui_style.hpp"
#include "ui_widgets.hpp"

namespace
{
lv_obj_t *s_root = nullptr;
lv_obj_t *s_title = nullptr;
lv_obj_t *s_subtitle = nullptr;
lv_obj_t *s_card = nullptr;

bool s_has_rendered_device = false;
char s_rendered_uid[GW_DEVICE_UID_STRLEN] = {0};
uint16_t s_rendered_short_addr = 0;
size_t s_rendered_endpoint_count = 0;
size_t s_rendered_active_endpoint_idx = 0;
uint32_t s_rendered_signature = 0;

uint32_t fnv1a_hash_u32(uint32_t hash, uint32_t value)
{
    hash ^= value;
    hash *= 16777619u;
    return hash;
}

uint32_t calc_device_signature(const ui_device_vm_t *dev)
{
    if (!dev)
    {
        return 0;
    }
    uint32_t hash = 2166136261u;
    hash = fnv1a_hash_u32(hash, (uint32_t)dev->endpoint_count);
    for (size_t i = 0; i < dev->endpoint_count; ++i)
    {
        const ui_endpoint_vm_t *ep = &dev->endpoints[i];
        hash = fnv1a_hash_u32(hash, ep->endpoint_id);
        hash = fnv1a_hash_u32(hash, ep->caps.onoff ? 1u : 0u);
        hash = fnv1a_hash_u32(hash, ep->caps.level ? 1u : 0u);
        hash = fnv1a_hash_u32(hash, ep->caps.color ? 1u : 0u);
        hash = fnv1a_hash_u32(hash, ep->caps.temperature ? 1u : 0u);
        hash = fnv1a_hash_u32(hash, ep->caps.humidity ? 1u : 0u);
        hash = fnv1a_hash_u32(hash, ep->caps.battery ? 1u : 0u);
        hash = fnv1a_hash_u32(hash, ep->caps.occupancy ? 1u : 0u);
    }
    return hash;
}

bool needs_rebuild(const ui_device_vm_t *dev)
{
    if (!dev)
    {
        return false;
    }
    if (!s_has_rendered_device)
    {
        return true;
    }
    if (strncmp(s_rendered_uid, dev->uid.uid, sizeof(s_rendered_uid)) != 0)
    {
        return true;
    }
    if (s_rendered_short_addr != dev->short_addr)
    {
        return true;
    }
    if (s_rendered_endpoint_count != dev->endpoint_count)
    {
        return true;
    }
    if (s_rendered_active_endpoint_idx != dev->active_endpoint_idx)
    {
        return true;
    }
    const uint32_t signature = calc_device_signature(dev);
    if (s_rendered_signature != signature)
    {
        return true;
    }
    return false;
}

void relayout_card_under_subtitle(void)
{
    if (!s_root || !s_subtitle || !s_card)
    {
        return;
    }

    lv_obj_align_to(s_card, s_subtitle, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_update_layout(s_root);

    const int32_t root_h = lv_obj_get_height(s_root);
    const int32_t card_y = lv_obj_get_y(s_card);
    int32_t card_h = root_h - card_y - ui_style::kCardBottomOffset;
    if (card_h < 40)
    {
        card_h = 40;
    }
    lv_obj_set_height(s_card, card_h);
}

void apply_field_bool(const char *device_uid, uint8_t endpoint, const char *key, bool has_value, bool value)
{
    ui_widget_value_t v = {};
    v.type = UI_WIDGET_VALUE_BOOL;
    v.has_value = has_value;
    v.v.b = value;
    (void)ui_widgets_set_state(device_uid, endpoint, key, &v);
}

void apply_field_u32(const char *device_uid, uint8_t endpoint, const char *key, bool has_value, uint32_t value)
{
    ui_widget_value_t v = {};
    v.type = UI_WIDGET_VALUE_U32;
    v.has_value = has_value;
    v.v.u32 = value;
    (void)ui_widgets_set_state(device_uid, endpoint, key, &v);
}

void apply_field_f32(const char *device_uid, uint8_t endpoint, const char *key, bool has_value, float value)
{
    ui_widget_value_t v = {};
    v.type = UI_WIDGET_VALUE_F32;
    v.has_value = has_value;
    v.v.f32 = value;
    (void)ui_widgets_set_state(device_uid, endpoint, key, &v);
}

void sync_endpoint_values(const ui_device_vm_t *dev, const ui_endpoint_vm_t *ep)
{
    if (!dev || !ep)
    {
        return;
    }
    const char *uid = dev->uid.uid;
    const uint8_t endpoint = ep->endpoint_id;

    if (ep->caps.onoff)
    {
        apply_field_bool(uid, endpoint, "onoff", ep->has_onoff, ep->onoff);
    }
    if (ep->caps.level)
    {
        apply_field_u32(uid, endpoint, "level", ep->has_level, ep->has_level ? ep->level : 0);
    }
    if (ep->caps.color)
    {
        apply_field_u32(uid, endpoint, "color_x", ep->has_color_x, ep->color_x);
        apply_field_u32(uid, endpoint, "color_y", ep->has_color_y, ep->color_y);
        apply_field_u32(uid, endpoint, "color_temp_mireds", ep->has_color_temp_mireds, ep->color_temp_mireds);
    }
    if (ep->caps.temperature)
    {
        apply_field_f32(uid, endpoint, "temperature_c", ep->has_temperature_c, ep->temperature_c);
    }
    if (ep->caps.humidity)
    {
        apply_field_f32(uid, endpoint, "humidity_pct", ep->has_humidity_pct, ep->humidity_pct);
    }
    if (ep->caps.battery)
    {
        apply_field_u32(uid, endpoint, "battery_pct", ep->has_battery_pct, ep->battery_pct);
    }
}

void sync_active_endpoint_values(const ui_device_vm_t *dev)
{
    if (!dev || dev->endpoint_count == 0)
    {
        return;
    }
    size_t ep_idx = dev->active_endpoint_idx;
    if (ep_idx >= dev->endpoint_count)
    {
        ep_idx = 0;
    }
    sync_endpoint_values(dev, &dev->endpoints[ep_idx]);
}

} // namespace

void ui_screen_devices_init(lv_obj_t *root)
{
    s_root = root;
    s_title = lv_label_create(root);
    s_subtitle = lv_label_create(root);

    lv_obj_set_style_bg_color(root, lv_color_hex(ui_style::kScreenBgHex), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, ui_style::kTitleY);
    lv_obj_set_style_text_color(s_title, lv_color_hex(ui_style::kTitleTextHex), 0);
    lv_obj_set_style_text_font(s_title, ui_style::kFontTitle, 0);

    // subtitle
    lv_obj_align_to(s_subtitle, s_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(ui_style::kSubtitleTextHex), 0);
    lv_obj_set_style_text_font(s_subtitle, ui_style::kFontSubtitle, 0);

}

void ui_screen_devices_apply_state_event(const ui_store_t *store, const gw_event_t *event)
{
    if (!store || !event || !s_root || !s_title || !s_subtitle)
    {
        return;
    }
    if (!s_has_rendered_device || event->device_uid[0] == '\0')
    {
        return;
    }
    if (strncmp(s_rendered_uid, event->device_uid, sizeof(s_rendered_uid)) != 0)
    {
        return;
    }
    const ui_device_vm_t *dev = ui_store_active_device(store);
    if (!dev || dev->endpoint_count == 0)
    {
        return;
    }
    size_t ep_idx = dev->active_endpoint_idx;
    if (ep_idx >= dev->endpoint_count)
    {
        ep_idx = 0;
    }
    const uint8_t active_ep = dev->endpoints[ep_idx].endpoint_id;

    if ((event->payload_flags & GW_EVENT_PAYLOAD_HAS_ENDPOINT) == 0 ||
        (event->payload_flags & GW_EVENT_PAYLOAD_HAS_CLUSTER) == 0 ||
        (event->payload_flags & GW_EVENT_PAYLOAD_HAS_ATTR) == 0 ||
        (event->payload_flags & GW_EVENT_PAYLOAD_HAS_VALUE) == 0)
    {
        return;
    }

    const uint8_t endpoint = event->payload_endpoint;
    if (endpoint != active_ep)
    {
        return;
    }
    const uint16_t cluster = event->payload_cluster;
    const uint16_t attr = event->payload_attr;
    const uint8_t value_type = event->payload_value_type;

    ui_widget_value_t value = {};
    const char *key = nullptr;

    if (cluster == 0x0006 && attr == 0x0000)
    {
        key = "onoff";
        value.type = UI_WIDGET_VALUE_BOOL;
        value.has_value = true;
        if (value_type == GW_EVENT_VALUE_BOOL)
        {
            value.v.b = (event->payload_value_bool != 0);
        }
        else if (value_type == GW_EVENT_VALUE_I64)
        {
            value.v.b = (event->payload_value_i64 != 0);
        }
        else if (value_type == GW_EVENT_VALUE_F64)
        {
            value.v.b = (event->payload_value_f64 != 0.0);
        }
        else
        {
            return;
        }
    }
    else if (cluster == 0x0008 && attr == 0x0000 && value_type == GW_EVENT_VALUE_I64)
    {
        key = "level";
        value.type = UI_WIDGET_VALUE_U32;
        value.has_value = true;
        value.v.u32 = (uint32_t)event->payload_value_i64;
    }
    else if (cluster == 0x0402 && attr == 0x0000)
    {
        key = "temperature_c";
        value.type = UI_WIDGET_VALUE_F32;
        value.has_value = true;
        if (value_type == GW_EVENT_VALUE_F64)
        {
            value.v.f32 = (float)event->payload_value_f64;
        }
        else if (value_type == GW_EVENT_VALUE_I64)
        {
            value.v.f32 = ((float)event->payload_value_i64) / 100.0f;
        }
        else
        {
            return;
        }
    }
    else if (cluster == 0x0405 && attr == 0x0000)
    {
        key = "humidity_pct";
        value.type = UI_WIDGET_VALUE_F32;
        value.has_value = true;
        if (value_type == GW_EVENT_VALUE_F64)
        {
            value.v.f32 = (float)event->payload_value_f64;
        }
        else if (value_type == GW_EVENT_VALUE_I64)
        {
            value.v.f32 = ((float)event->payload_value_i64) / 100.0f;
        }
        else
        {
            return;
        }
    }
    else if (cluster == 0x0001 && attr == 0x0021 && value_type == GW_EVENT_VALUE_I64)
    {
        key = "battery_pct";
        value.type = UI_WIDGET_VALUE_U32;
        value.has_value = true;
        value.v.u32 = (uint32_t)event->payload_value_i64;
    }
    else if (cluster == 0x0300 && attr == 0x0003 && value_type == GW_EVENT_VALUE_I64)
    {
        key = "color_x";
        value.type = UI_WIDGET_VALUE_U32;
        value.has_value = true;
        value.v.u32 = (uint32_t)event->payload_value_i64;
    }
    else if (cluster == 0x0300 && attr == 0x0004 && value_type == GW_EVENT_VALUE_I64)
    {
        key = "color_y";
        value.type = UI_WIDGET_VALUE_U32;
        value.has_value = true;
        value.v.u32 = (uint32_t)event->payload_value_i64;
    }
    else if (cluster == 0x0300 && attr == 0x0007 && value_type == GW_EVENT_VALUE_I64)
    {
        key = "color_temp_mireds";
        value.type = UI_WIDGET_VALUE_U32;
        value.has_value = true;
        value.v.u32 = (uint32_t)event->payload_value_i64;
    }
    else
    {
        return;
    }

    (void)ui_widgets_set_state(event->device_uid, endpoint, key, &value);
}

void ui_screen_devices_render(const ui_store_t *store)
{
    if (!store || !s_root || !s_title || !s_subtitle)
    {
        return;
    }

    const ui_device_vm_t *dev = ui_store_active_device(store);
    if (!dev)
    {
        lv_label_set_text(s_title, "No devices");
        lv_label_set_text(s_subtitle, "Rotate encoder after devices join");
        lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, ui_style::kTitleY);
        lv_obj_align_to(s_subtitle, s_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
        if (s_card)
        {
            lv_obj_delete(s_card);
            s_card = nullptr;
        }
        ui_widgets_reset();
        s_has_rendered_device = false;
        memset(s_rendered_uid, 0, sizeof(s_rendered_uid));
        s_rendered_short_addr = 0;
        s_rendered_endpoint_count = 0;
        s_rendered_active_endpoint_idx = 0;
        s_rendered_signature = 0;
        return;
    }

    size_t active_ep_idx = dev->active_endpoint_idx;
    if (active_ep_idx >= dev->endpoint_count)
    {
        active_ep_idx = 0;
    }
    const ui_endpoint_vm_t *active_ep = (dev->endpoint_count > 0) ? &dev->endpoints[active_ep_idx] : nullptr;

    // lv_label_set_text_fmt(s_title, "%s (%s)", dev->name[0] ? dev->name : "device", dev->uid.uid);
    lv_label_set_text_fmt(s_title, "%s", dev->name[0] ? dev->name : "device");
    const char *ep_kind = (active_ep && active_ep->kind[0]) ? active_ep->kind : "endpoint";
    lv_label_set_text_fmt(s_subtitle,
                          "#%u/%u - %s",
                          (unsigned)(active_ep_idx + 1),
                          (unsigned)dev->endpoint_count,
                          ep_kind);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, ui_style::kTitleY);
    lv_obj_align_to(s_subtitle, s_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    if (s_card)
    {
        relayout_card_under_subtitle();
    }

    if (needs_rebuild(dev))
    {
        if (s_card)
        {
            lv_obj_delete(s_card);
            s_card = nullptr;
        }
        ui_widgets_reset();
        if (active_ep)
        {
            s_card = ui_widgets_create_endpoint_card(s_root, dev, active_ep);
            if (s_card)
            {
                lv_obj_set_width(s_card, ui_style::kListWidth);
                relayout_card_under_subtitle();
            }
        }
        s_has_rendered_device = true;
        strlcpy(s_rendered_uid, dev->uid.uid, sizeof(s_rendered_uid));
        s_rendered_short_addr = dev->short_addr;
        s_rendered_endpoint_count = dev->endpoint_count;
        s_rendered_active_endpoint_idx = active_ep_idx;
        s_rendered_signature = calc_device_signature(dev);
        sync_active_endpoint_values(dev);
    }
}

