#include "ui_screen_devices.hpp"

#include <stdio.h>
#include <string.h>

#include "ui_actions.hpp"

namespace
{
lv_obj_t *s_root = nullptr;
lv_obj_t *s_title = nullptr;
lv_obj_t *s_subtitle = nullptr;
lv_obj_t *s_list = nullptr;

enum class CtlKind : uint8_t
{
    OnOff = 1,
    Level = 2,
    ColorTemp = 3,
};

typedef struct
{
    CtlKind kind;
    gw_device_uid_t uid;
    uint8_t endpoint;
} ui_ctl_ctx_t;

static ui_ctl_ctx_t s_ctx_pool[64];
static size_t s_ctx_count = 0;

ui_ctl_ctx_t *alloc_ctx(CtlKind kind, const gw_device_uid_t *uid, uint8_t endpoint)
{
    if (!uid || s_ctx_count >= (sizeof(s_ctx_pool) / sizeof(s_ctx_pool[0])))
    {
        return nullptr;
    }
    ui_ctl_ctx_t *ctx = &s_ctx_pool[s_ctx_count++];
    memset(ctx, 0, sizeof(*ctx));
    ctx->kind = kind;
    ctx->uid = *uid;
    ctx->endpoint = endpoint;
    return ctx;
}

void on_switch_changed(lv_event_t *e)
{
    ui_ctl_ctx_t *ctx = (ui_ctl_ctx_t *)lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    const bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    (void)ui_actions_enqueue_onoff(&ctx->uid, ctx->endpoint, checked);
}

void on_slider_released(lv_event_t *e)
{
    ui_ctl_ctx_t *ctx = (ui_ctl_ctx_t *)lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    const int32_t value = lv_slider_get_value(slider);
    (void)ui_actions_enqueue_level(&ctx->uid, ctx->endpoint, (uint8_t)value);
}

void on_color_temp_clicked(lv_event_t *e)
{
    ui_ctl_ctx_t *ctx = (ui_ctl_ctx_t *)lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }
    (void)ui_actions_enqueue_color_temp(&ctx->uid, ctx->endpoint, 370);
}

void add_endpoint_card(const ui_device_vm_t *dev, const ui_endpoint_vm_t *ep)
{
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16223f), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2f3c63), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *hdr = lv_label_create(card);
    lv_label_set_text_fmt(hdr, "EP%u (%s)", (unsigned)ep->endpoint_id, ep->kind[0] ? ep->kind : "endpoint");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xe5e7eb), 0);

    if (ep->caps.onoff)
    {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "On/Off");
        lv_obj_t *sw = lv_switch_create(row);
        if (ep->has_onoff && ep->onoff)
        {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        ui_ctl_ctx_t *ctx = alloc_ctx(CtlKind::OnOff, &dev->uid, ep->endpoint_id);
        lv_obj_add_event_cb(sw, on_switch_changed, LV_EVENT_VALUE_CHANGED, ctx);
    }

    if (ep->caps.level)
    {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text_fmt(lbl, "Level: %u", (unsigned)(ep->has_level ? ep->level : 0));
        lv_obj_t *slider = lv_slider_create(card);
        lv_obj_set_width(slider, lv_pct(100));
        lv_slider_set_range(slider, 0, 254);
        lv_slider_set_value(slider, (int32_t)(ep->has_level ? ep->level : 0), LV_ANIM_OFF);
        ui_ctl_ctx_t *ctx = alloc_ctx(CtlKind::Level, &dev->uid, ep->endpoint_id);
        lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED, ctx);
    }

    if (ep->caps.color)
    {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text_fmt(lbl,
                              "Color: X=%u Y=%u T=%u",
                              (unsigned)(ep->has_color_x ? ep->color_x : 0),
                              (unsigned)(ep->has_color_y ? ep->color_y : 0),
                              (unsigned)(ep->has_color_temp_mireds ? ep->color_temp_mireds : 0));
        lv_obj_t *btn = lv_button_create(card);
        lv_obj_t *btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "Set Warm (370)");
        lv_obj_center(btn_lbl);
        ui_ctl_ctx_t *ctx = alloc_ctx(CtlKind::ColorTemp, &dev->uid, ep->endpoint_id);
        lv_obj_add_event_cb(btn, on_color_temp_clicked, LV_EVENT_CLICKED, ctx);
    }

    if (ep->caps.temperature)
    {
        lv_obj_t *lbl = lv_label_create(card);
        if (ep->has_temperature_c)
        {
            lv_label_set_text_fmt(lbl, "Temperature: %.1f C", ep->temperature_c);
        }
        else
        {
            lv_label_set_text(lbl, "Temperature: -");
        }
    }

    if (ep->caps.humidity)
    {
        lv_obj_t *lbl = lv_label_create(card);
        if (ep->has_humidity_pct)
        {
            lv_label_set_text_fmt(lbl, "Humidity: %.1f %%", ep->humidity_pct);
        }
        else
        {
            lv_label_set_text(lbl, "Humidity: -");
        }
    }

    if (ep->caps.battery)
    {
        lv_obj_t *lbl = lv_label_create(card);
        if (ep->has_battery_pct)
        {
            lv_label_set_text_fmt(lbl, "Battery: %u %%", (unsigned)ep->battery_pct);
        }
        else
        {
            lv_label_set_text(lbl, "Battery: -");
        }
    }
}
} // namespace

void ui_screen_devices_init(lv_obj_t *root)
{
    s_root = root;
    s_title = lv_label_create(root);
    s_subtitle = lv_label_create(root);
    s_list = lv_obj_create(root);

    lv_obj_set_style_bg_color(root, lv_color_hex(0x0b1020), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 12, 10);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_24, 0);

    lv_obj_align(s_subtitle, LV_ALIGN_TOP_LEFT, 12, 44);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(0x9fb0d9), 0);

    lv_obj_set_size(s_list, 456, 250);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(0x2f3c63), 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
}

void ui_screen_devices_render(const ui_store_t *store)
{
    if (!store || !s_root || !s_list || !s_title || !s_subtitle)
    {
        return;
    }

    s_ctx_count = 0;
    lv_obj_clean(s_list);

    const ui_device_vm_t *dev = ui_store_active_device(store);
    if (!dev)
    {
        lv_label_set_text(s_title, "No devices");
        lv_label_set_text(s_subtitle, "Rotate encoder after devices join");
        return;
    }

    lv_label_set_text_fmt(s_title, "%s (%s)", dev->name[0] ? dev->name : "device", dev->uid.uid);
    lv_label_set_text_fmt(s_subtitle,
                          "Device %u/%u  short: 0x%04X  endpoints: %u",
                          (unsigned)(store->active_device_idx + 1),
                          (unsigned)store->device_count,
                          (unsigned)dev->short_addr,
                          (unsigned)dev->endpoint_count);

    for (size_t i = 0; i < dev->endpoint_count; ++i)
    {
        add_endpoint_card(dev, &dev->endpoints[i]);
    }
}

