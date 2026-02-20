#include "ui_widgets.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gw_core/types.h"
#include "ui_actions.hpp"
#include "ui_control_ack.hpp"
#include "ui_style.hpp"

namespace
{
enum class CtlKind : uint8_t
{
    OnOff = 1,
    Level = 2,
    ColorHS = 3,
};

typedef struct
{
    CtlKind kind;
    gw_device_uid_t uid;
    uint8_t endpoint;
    lv_obj_t *slider_hue;
    lv_obj_t *slider_sat;
    lv_obj_t *label_hue;
    lv_obj_t *label_sat;
    uint16_t cached_hue;
    uint8_t cached_sat;
    bool has_cached_hs;
} ui_ctl_ctx_t;

enum class FieldKind : uint8_t
{
    Switch = 1,
    Slider = 2,
    ValueLabel = 3,
    ColorState = 4,
};

typedef struct
{
    bool used;
    char device_uid[GW_DEVICE_UID_STRLEN];
    uint8_t endpoint;
    char key[24];
    FieldKind kind;
    lv_obj_t *obj;
    lv_obj_t *aux_obj;
    uint16_t color_x;
    uint16_t color_y;
    bool has_color_x;
    bool has_color_y;
} field_entry_t;

static constexpr size_t kMaxCtlCtx = 96;
static constexpr size_t kMaxFieldEntries = 96;

static ui_ctl_ctx_t s_ctx_pool[kMaxCtlCtx];
static size_t s_ctx_count = 0;
static field_entry_t s_fields[kMaxFieldEntries];
static size_t s_field_count = 0;
static lv_grad_dsc_t s_hue_grad = {};
static bool s_hue_grad_ready = false;
static constexpr lv_state_t kStateError = LV_STATE_USER_1;
static constexpr lv_style_selector_t kSelMainError =
    static_cast<lv_style_selector_t>(static_cast<uint32_t>(LV_PART_MAIN) |
                                     static_cast<uint32_t>(kStateError));

void note_user_activity_local(void)
{
    lv_display_t *display = lv_display_get_default();
    if (display) {
        lv_display_trigger_activity(display);
    }
}

void on_touch_activity(lv_event_t *e)
{
    (void)e;
    note_user_activity_local();
}

void ensure_hue_gradient(void)
{
    if (s_hue_grad_ready)
    {
        return;
    }

    lv_memzero(&s_hue_grad, sizeof(s_hue_grad));
    s_hue_grad.dir = LV_GRAD_DIR_HOR;
    s_hue_grad.extend = LV_GRAD_EXTEND_PAD;
    s_hue_grad.stops_count = 7;

    s_hue_grad.stops[0].frac = 0;
    s_hue_grad.stops[0].color = lv_palette_main(LV_PALETTE_RED);
    s_hue_grad.stops[0].opa = LV_OPA_COVER;

    s_hue_grad.stops[1].frac = 42;
    s_hue_grad.stops[1].color = lv_palette_main(LV_PALETTE_ORANGE);
    s_hue_grad.stops[1].opa = LV_OPA_COVER;

    s_hue_grad.stops[2].frac = 85;
    s_hue_grad.stops[2].color = lv_palette_main(LV_PALETTE_YELLOW);
    s_hue_grad.stops[2].opa = LV_OPA_COVER;

    s_hue_grad.stops[3].frac = 128;
    s_hue_grad.stops[3].color = lv_palette_main(LV_PALETTE_GREEN);
    s_hue_grad.stops[3].opa = LV_OPA_COVER;

    s_hue_grad.stops[4].frac = 170;
    s_hue_grad.stops[4].color = lv_palette_main(LV_PALETTE_BLUE);
    s_hue_grad.stops[4].opa = LV_OPA_COVER;

    s_hue_grad.stops[5].frac = 213;
    s_hue_grad.stops[5].color = lv_palette_main(LV_PALETTE_PURPLE);
    s_hue_grad.stops[5].opa = LV_OPA_COVER;

    s_hue_grad.stops[6].frac = 255;
    s_hue_grad.stops[6].color = lv_palette_main(LV_PALETTE_RED);
    s_hue_grad.stops[6].opa = LV_OPA_COVER;

    s_hue_grad_ready = true;
}

void apply_hue_slider_style(lv_obj_t *slider)
{
    if (!slider)
    {
        return;
    }

    ensure_hue_gradient();

    lv_obj_set_height(slider, 18);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad(slider, &s_hue_grad, LV_PART_MAIN);

    // Keep track fully visible; knob indicates current hue position.
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(0x111827), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 2, LV_PART_KNOB);
}

void set_value_1dp(lv_obj_t *label, const char *prefix, float v, const char *suffix)
{
    if (!label || !prefix || !suffix)
    {
        return;
    }

    // Avoid LVGL float formatter dependency (%f may print literal 'f' when disabled).
    const float scaled_f = v * 10.0f;
    const int32_t scaled = (scaled_f >= 0.0f) ? (int32_t)(scaled_f + 0.5f) : (int32_t)(scaled_f - 0.5f);
    int32_t abs_scaled = (scaled < 0) ? -scaled : scaled;
    const int32_t whole = abs_scaled / 10;
    const int32_t frac = abs_scaled % 10;
    lv_label_set_text_fmt(label, "%s%s%ld.%ld%s", prefix, (scaled < 0) ? "-" : "", (long)whole, (long)frac, suffix);
}

ui_ctl_ctx_t *alloc_ctx(CtlKind kind, const gw_device_uid_t *uid, uint8_t endpoint)
{
    if (!uid || s_ctx_count >= kMaxCtlCtx)
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

void register_field(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, FieldKind kind, lv_obj_t *obj, lv_obj_t *aux_obj)
{
    if (!uid || !key || !key[0] || !obj || s_field_count >= kMaxFieldEntries)
    {
        return;
    }
    field_entry_t *entry = &s_fields[s_field_count++];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    strlcpy(entry->device_uid, uid->uid, sizeof(entry->device_uid));
    strlcpy(entry->key, key, sizeof(entry->key));
    entry->endpoint = endpoint;
    entry->kind = kind;
    entry->obj = obj;
    entry->aux_obj = aux_obj;
}

field_entry_t *find_field(const char *device_uid, uint8_t endpoint, const char *key)
{
    if (!device_uid || !key)
    {
        return nullptr;
    }
    for (size_t i = 0; i < s_field_count; ++i)
    {
        field_entry_t *entry = &s_fields[i];
        if (!entry->used)
        {
            continue;
        }
        if (entry->endpoint != endpoint)
        {
            continue;
        }
        if (strncmp(entry->device_uid, device_uid, sizeof(entry->device_uid)) != 0)
        {
            continue;
        }
        if (strncmp(entry->key, key, sizeof(entry->key)) != 0)
        {
            continue;
        }
        return entry;
    }
    return nullptr;
}

ui_ctl_ctx_t *find_color_ctx(const char *device_uid, uint8_t endpoint)
{
    if (!device_uid)
    {
        return nullptr;
    }
    for (size_t i = 0; i < s_ctx_count; ++i)
    {
        ui_ctl_ctx_t *ctx = &s_ctx_pool[i];
        if (ctx->kind != CtlKind::ColorHS)
        {
            continue;
        }
        if (ctx->endpoint != endpoint)
        {
            continue;
        }
        if (strncmp(ctx->uid.uid, device_uid, sizeof(ctx->uid.uid)) != 0)
        {
            continue;
        }
        return ctx;
    }
    return nullptr;
}

float clamp01(float v)
{
    if (v < 0.0f)
    {
        return 0.0f;
    }
    if (v > 1.0f)
    {
        return 1.0f;
    }
    return v;
}

float linear_to_srgb(float c)
{
    if (c <= 0.0031308f)
    {
        return 12.92f * c;
    }
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

float srgb_to_linear(float c)
{
    if (c <= 0.04045f)
    {
        return c / 12.92f;
    }
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

lv_color_t color_from_xy(uint16_t x_raw, uint16_t y_raw)
{
    const float x = ((float)x_raw) / 65535.0f;
    const float y = ((float)y_raw) / 65535.0f;
    if (y <= 0.0001f)
    {
        return lv_color_hex(0x202020);
    }

    const float Y = 1.0f;
    const float X = (x * Y) / y;
    const float Z = ((1.0f - x - y) * Y) / y;

    float r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    float b = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;

    if (r < 0.0f)
    {
        r = 0.0f;
    }
    if (g < 0.0f)
    {
        g = 0.0f;
    }
    if (b < 0.0f)
    {
        b = 0.0f;
    }

    const float max_rgb = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    if (max_rgb > 1.0f)
    {
        r /= max_rgb;
        g /= max_rgb;
        b /= max_rgb;
    }

    r = clamp01(linear_to_srgb(r));
    g = clamp01(linear_to_srgb(g));
    b = clamp01(linear_to_srgb(b));

    const uint8_t r8 = (uint8_t)(r * 255.0f + 0.5f);
    const uint8_t g8 = (uint8_t)(g * 255.0f + 0.5f);
    const uint8_t b8 = (uint8_t)(b * 255.0f + 0.5f);
    return lv_color_make(r8, g8, b8);
}

void xy_from_hsv(uint16_t hue, uint8_t sat, uint16_t *out_x, uint16_t *out_y)
{
    if (!out_x || !out_y)
    {
        return;
    }

    lv_color_t rgb = lv_color_hsv_to_rgb(hue, sat, 100);
    lv_color32_t rgb32 = lv_color_to_32(rgb, LV_OPA_COVER);
    const float r = srgb_to_linear(((float)rgb32.red) / 255.0f);
    const float g = srgb_to_linear(((float)rgb32.green) / 255.0f);
    const float b = srgb_to_linear(((float)rgb32.blue) / 255.0f);

    const float X = 0.4124f * r + 0.3576f * g + 0.1805f * b;
    const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float Z = 0.0193f * r + 0.1192f * g + 0.9505f * b;
    const float sum = X + Y + Z;

    float x = 0.3127f;
    float y = 0.3290f;
    if (sum > 0.000001f)
    {
        x = X / sum;
        y = Y / sum;
    }

    x = clamp01(x);
    y = clamp01(y);
    *out_x = (uint16_t)(x * 65535.0f + 0.5f);
    *out_y = (uint16_t)(y * 65535.0f + 0.5f);
}

void refresh_color_controls(ui_ctl_ctx_t *ctx, uint16_t hue, uint8_t sat, bool update_sliders)
{
    if (!ctx)
    {
        return;
    }

    const bool hue_changed = (!ctx->has_cached_hs) || (ctx->cached_hue != hue);
    const bool sat_changed = (!ctx->has_cached_hs) || (ctx->cached_sat != sat);
    if (!update_sliders && !hue_changed && !sat_changed)
    {
        return;
    }

    if (ctx->label_hue && hue_changed)
    {
        lv_label_set_text_fmt(ctx->label_hue, "Hue: %u", (unsigned)hue);
    }
    if (ctx->label_sat && sat_changed)
    {
        lv_label_set_text_fmt(ctx->label_sat, "Saturation: %u %%", (unsigned)sat);
    }

    if (ctx->slider_hue)
    {
        if (update_sliders)
        {
            lv_slider_set_value(ctx->slider_hue, (int32_t)hue, LV_ANIM_OFF);
        }
    }

    if (ctx->slider_sat)
    {
        if (update_sliders)
        {
            lv_slider_set_value(ctx->slider_sat, (int32_t)sat, LV_ANIM_OFF);
        }
    }

    ctx->cached_hue = hue;
    ctx->cached_sat = sat;
    ctx->has_cached_hs = true;
}

void on_switch_tapped(lv_event_t *e)
{
    note_user_activity_local();
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) {
        return;
    }
    ui_ctl_ctx_t *ctx = (ui_ctl_ctx_t *)lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }
    field_entry_t *entry = find_field(ctx->uid.uid, ctx->endpoint, "onoff");
    if (!entry)
    {
        return;
    }

    // Ignore user taps while command is in-flight.
    if (ui_control_ack_is_pending(ctx->uid.uid, ctx->endpoint, "onoff"))
    {
        return;
    }

    bool has_confirmed = false;
    bool confirmed = false;
    (void)ui_control_ack_get_confirmed_bool(ctx->uid.uid, ctx->endpoint, "onoff", &has_confirmed, &confirmed);
    const bool target = has_confirmed ? !confirmed : true;

    if (!ui_control_ack_begin(ctx->uid.uid, ctx->endpoint, "onoff"))
    {
        return;
    }
    lv_obj_remove_state(entry->obj, kStateError);
    lv_obj_add_state(entry->obj, LV_STATE_DISABLED);

    const esp_err_t err = ui_actions_enqueue_onoff(&ctx->uid, ctx->endpoint, target);
    if (err != ESP_OK)
    {
        // Send failed immediately; unlock control and keep confirmed state.
        ui_control_ack_fail(ctx->uid.uid, ctx->endpoint, "onoff");
        lv_obj_remove_state(entry->obj, LV_STATE_DISABLED);
        lv_obj_add_state(entry->obj, kStateError);
    }
}

void on_slider_released(lv_event_t *e)
{
    note_user_activity_local();
    ui_ctl_ctx_t *ctx = (ui_ctl_ctx_t *)lv_event_get_user_data(e);
    if (!ctx)
    {
        return;
    }
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    const int32_t value = lv_slider_get_value(slider);
    (void)ui_actions_enqueue_level(&ctx->uid, ctx->endpoint, (uint8_t)value);
}

void on_color_hs_released(lv_event_t *e)
{
    note_user_activity_local();
    ui_ctl_ctx_t *ctx = (ui_ctl_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->slider_hue || !ctx->slider_sat)
    {
        return;
    }
    const int32_t hue = lv_slider_get_value(ctx->slider_hue);
    const int32_t sat = lv_slider_get_value(ctx->slider_sat);
    if (hue < 0 || sat < 0)
    {
        return;
    }
    uint16_t x = 0;
    uint16_t y = 0;
    xy_from_hsv((uint16_t)hue, (uint8_t)sat, &x, &y);
    (void)ui_actions_enqueue_color_xy(&ctx->uid, ctx->endpoint, x, y);
}

void switch_set_state(field_entry_t *entry, const ui_widget_value_t *value)
{
    if (!entry || !entry->obj || !value || value->type != UI_WIDGET_VALUE_BOOL)
    {
        return;
    }

    ui_control_ack_confirm_bool(entry->device_uid, entry->endpoint, "onoff", value->has_value, value->has_value && value->v.b);
    const ui_control_ack_status_t st = ui_control_ack_get_status(entry->device_uid, entry->endpoint, "onoff");
    if (st == UI_CONTROL_ACK_PENDING) {
        lv_obj_add_state(entry->obj, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(entry->obj, LV_STATE_DISABLED);
    }
    if (st == UI_CONTROL_ACK_ERROR) {
        lv_obj_add_state(entry->obj, kStateError);
    } else {
        lv_obj_remove_state(entry->obj, kStateError);
    }

    if (value->has_value && value->v.b)
    {
        lv_obj_add_state(entry->obj, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_remove_state(entry->obj, LV_STATE_CHECKED);
    }
}

void slider_set_state(field_entry_t *entry, const ui_widget_value_t *value)
{
    if (!entry || !entry->obj || !entry->aux_obj || !value || value->type != UI_WIDGET_VALUE_U32)
    {
        return;
    }
    const uint32_t level = value->has_value ? value->v.u32 : 0;
    lv_slider_set_value(entry->obj, (int32_t)level, LV_ANIM_OFF);
    lv_label_set_text_fmt(entry->aux_obj, "Level: %u", (unsigned)level);
}

void color_state_set_state(field_entry_t *entry, const ui_widget_value_t *value)
{
    if (!entry || !value || value->type != UI_WIDGET_VALUE_U32)
    {
        return;
    }
    if (!value->has_value)
    {
        return;
    }
    if (strcmp(entry->key, "color_x") == 0)
    {
        entry->has_color_x = true;
        entry->color_x = (uint16_t)value->v.u32;
    }
    else if (strcmp(entry->key, "color_y") == 0)
    {
        entry->has_color_y = true;
        entry->color_y = (uint16_t)value->v.u32;
    }

    field_entry_t *x_entry = find_field(entry->device_uid, entry->endpoint, "color_x");
    field_entry_t *y_entry = find_field(entry->device_uid, entry->endpoint, "color_y");
    if (!x_entry || !y_entry || !x_entry->has_color_x || !y_entry->has_color_y)
    {
        return;
    }

    ui_ctl_ctx_t *ctx = find_color_ctx(entry->device_uid, entry->endpoint);
    if (!ctx)
    {
        return;
    }
    lv_color_t xy_color = color_from_xy(x_entry->color_x, y_entry->color_y);
    lv_color_hsv_t hsv = lv_color_to_hsv(xy_color);
    refresh_color_controls(ctx, hsv.h, hsv.s, true);
}

void value_label_set_state(field_entry_t *entry, const ui_widget_value_t *value)
{
    if (!entry || !entry->obj || !value)
    {
        return;
    }

    if (strncmp(entry->key, "temperature_c", sizeof(entry->key)) == 0)
    {
        if (value->has_value && value->type == UI_WIDGET_VALUE_F32)
        {
            set_value_1dp(entry->obj, "", value->v.f32, " C");
        }
        else
        {
            lv_label_set_text(entry->obj, "-");
        }
        return;
    }

    if (strncmp(entry->key, "humidity_pct", sizeof(entry->key)) == 0)
    {
        if (value->has_value && value->type == UI_WIDGET_VALUE_F32)
        {
            set_value_1dp(entry->obj, "", value->v.f32, " %");
        }
        else
        {
            lv_label_set_text(entry->obj, "-");
        }
        return;
    }

    if (strncmp(entry->key, "battery_pct", sizeof(entry->key)) == 0)
    {
        if (value->has_value && value->type == UI_WIDGET_VALUE_U32)
        {
            lv_label_set_text_fmt(entry->obj, "Battery: %u %%", (unsigned)value->v.u32);
        }
        else
        {
            lv_label_set_text(entry->obj, "Battery: -");
        }
        return;
    }

    if (strncmp(entry->key, "color_x", sizeof(entry->key)) == 0 ||
        strncmp(entry->key, "color_y", sizeof(entry->key)) == 0)
    {
        // XY state drives HS sliders in color_state_set_state; no dedicated label output.
        return;
    }

}
} // namespace

void ui_widgets_reset(void)
{
    s_ctx_count = 0;
    s_field_count = 0;
    memset(s_ctx_pool, 0, sizeof(s_ctx_pool));
    memset(s_fields, 0, sizeof(s_fields));
    ui_control_ack_reset();
}

lv_obj_t *ui_widgets_create_endpoint_card(lv_obj_t *parent, const gw_device_uid_t *uid, const ui_endpoint_vm_t *ep)
{
    if (!parent || !uid || !ep)
    {
        return nullptr;
    }

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(card, ui_style::kCardPad, 0);
    lv_obj_set_style_pad_row(card, ui_style::kCardRowGap, 0);
    // Transparent container: keep only layout, no visual panel under widgets.
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_text_font(card, ui_style::kFontBody, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (ep->caps.onoff)
    {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 56);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "On/Off");
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 84, 46);
        lv_obj_set_style_border_width(sw, 2, kSelMainError);
        lv_obj_set_style_border_color(sw, lv_palette_main(LV_PALETTE_RED), kSelMainError);
        lv_obj_set_style_bg_color(sw, lv_palette_lighten(LV_PALETTE_RED, 4), kSelMainError);
        // Prevent native optimistic toggle; this control reflects only confirmed state.
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        ui_ctl_ctx_t *ctx = alloc_ctx(CtlKind::OnOff, uid, ep->endpoint_id);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_touch_activity, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(row, on_switch_tapped, LV_EVENT_SHORT_CLICKED, ctx);
        register_field(uid, ep->endpoint_id, "onoff", FieldKind::Switch, sw, nullptr);
    }

    if (ep->caps.level)
    {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, "Level: 0");
        lv_obj_t *slider = lv_slider_create(card);
        lv_obj_set_width(slider, lv_pct(100));
        lv_slider_set_range(slider, ui_style::kLevelMin, ui_style::kLevelMax);
        ui_ctl_ctx_t *ctx = alloc_ctx(CtlKind::Level, uid, ep->endpoint_id);
        lv_obj_add_event_cb(slider, on_touch_activity, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(slider, on_touch_activity, LV_EVENT_PRESSING, nullptr);
        lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED, ctx);
        register_field(uid, ep->endpoint_id, "level", FieldKind::Slider, slider, lbl);
    }

    if (ep->caps.color)
    {
        lv_obj_t *lbl_h = lv_label_create(card);
        lv_label_set_text(lbl_h, "Hue: 0");
        lv_obj_t *slider_h = lv_slider_create(card);
        lv_obj_set_width(slider_h, lv_pct(100));
        lv_slider_set_range(slider_h, 0, 359);
        apply_hue_slider_style(slider_h);

        lv_obj_t *lbl_s = lv_label_create(card);
        lv_label_set_text(lbl_s, "Saturation: 100 %");
        lv_obj_t *slider_s = lv_slider_create(card);
        lv_obj_set_width(slider_s, lv_pct(100));
        lv_slider_set_range(slider_s, 0, 100);
        lv_slider_set_value(slider_s, 100, LV_ANIM_OFF);

        ui_ctl_ctx_t *hs_ctx = alloc_ctx(CtlKind::ColorHS, uid, ep->endpoint_id);
        if (hs_ctx)
        {
            hs_ctx->slider_hue = slider_h;
            hs_ctx->slider_sat = slider_s;
            hs_ctx->label_hue = lbl_h;
            hs_ctx->label_sat = lbl_s;
            refresh_color_controls(hs_ctx, 0, 100, true);
            lv_obj_add_event_cb(slider_h, on_touch_activity, LV_EVENT_PRESSED, nullptr);
            lv_obj_add_event_cb(slider_h, on_touch_activity, LV_EVENT_PRESSING, nullptr);
            lv_obj_add_event_cb(slider_s, on_touch_activity, LV_EVENT_PRESSED, nullptr);
            lv_obj_add_event_cb(slider_s, on_touch_activity, LV_EVENT_PRESSING, nullptr);
            lv_obj_add_event_cb(slider_h, on_color_hs_released, LV_EVENT_RELEASED, hs_ctx);
            lv_obj_add_event_cb(slider_s, on_color_hs_released, LV_EVENT_RELEASED, hs_ctx);
        }

        // State keys still come as Zigbee xy; map them to this HS control.
        register_field(uid, ep->endpoint_id, "color_x", FieldKind::ColorState, slider_h, nullptr);
        register_field(uid, ep->endpoint_id, "color_y", FieldKind::ColorState, slider_h, nullptr);
    }

    if (ep->caps.temperature || ep->caps.humidity)
    {
        lv_obj_t *metrics_row = lv_obj_create(card);
        lv_obj_remove_style_all(metrics_row);
        lv_obj_set_width(metrics_row, lv_pct(100));
        lv_obj_set_height(metrics_row, LV_SIZE_CONTENT);
        lv_obj_clear_flag(metrics_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_column(metrics_row, 8, 0);
        lv_obj_set_flex_flow(metrics_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(metrics_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        if (ep->caps.temperature)
        {
            lv_obj_t *temp_card = lv_obj_create(metrics_row);
            lv_obj_set_size(temp_card, lv_pct(48), LV_SIZE_CONTENT);
            lv_obj_clear_flag(temp_card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_pad_all(temp_card, 8, 0);
            lv_obj_set_style_bg_color(temp_card, lv_color_hex(ui_style::kPanelBgHex), 0);
            lv_obj_set_style_border_color(temp_card, lv_color_hex(ui_style::kBorderHex), 0);
            lv_obj_set_style_radius(temp_card, 8, 0);
            lv_obj_set_flex_flow(temp_card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(temp_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

            lv_obj_t *temp_title = lv_label_create(temp_card);
            lv_label_set_text(temp_title, "Temperature");
            lv_obj_set_style_text_color(temp_title, lv_color_hex(ui_style::kSubtitleTextHex), 0);

            lv_obj_t *temp_value = lv_label_create(temp_card);
            lv_label_set_text(temp_value, "-");
            lv_obj_set_style_text_color(temp_value, lv_color_hex(ui_style::kTitleTextHex), 0);
            register_field(uid, ep->endpoint_id, "temperature_c", FieldKind::ValueLabel, temp_value, nullptr);
        }

        if (ep->caps.humidity)
        {
            lv_obj_t *hum_card = lv_obj_create(metrics_row);
            lv_obj_set_size(hum_card, lv_pct(48), LV_SIZE_CONTENT);
            lv_obj_clear_flag(hum_card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_pad_all(hum_card, 8, 0);
            lv_obj_set_style_bg_color(hum_card, lv_color_hex(ui_style::kPanelBgHex), 0);
            lv_obj_set_style_border_color(hum_card, lv_color_hex(ui_style::kBorderHex), 0);
            lv_obj_set_style_radius(hum_card, 8, 0);
            lv_obj_set_flex_flow(hum_card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(hum_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

            lv_obj_t *hum_title = lv_label_create(hum_card);
            lv_label_set_text(hum_title, "Humidity");
            lv_obj_set_style_text_color(hum_title, lv_color_hex(ui_style::kSubtitleTextHex), 0);

            lv_obj_t *hum_value = lv_label_create(hum_card);
            lv_label_set_text(hum_value, "-");
            lv_obj_set_style_text_color(hum_value, lv_color_hex(ui_style::kTitleTextHex), 0);
            register_field(uid, ep->endpoint_id, "humidity_pct", FieldKind::ValueLabel, hum_value, nullptr);
        }
    }

    if (ep->caps.battery)
    {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, "Battery: -");
        register_field(uid, ep->endpoint_id, "battery_pct", FieldKind::ValueLabel, lbl, nullptr);
    }

    return card;
}

bool ui_widgets_set_state(const char *device_uid, uint8_t endpoint, const char *key, const ui_widget_value_t *value)
{
    field_entry_t *entry = find_field(device_uid, endpoint, key);
    if (!entry || !value)
    {
        return false;
    }

    switch (entry->kind)
    {
    case FieldKind::Switch:
        switch_set_state(entry, value);
        break;
    case FieldKind::Slider:
        slider_set_state(entry, value);
        break;
    case FieldKind::ColorState:
        color_state_set_state(entry, value);
        break;
    case FieldKind::ValueLabel:
        value_label_set_state(entry, value);
        break;
    default:
        return false;
    }

    return true;
}

void ui_widgets_refresh_ack(const char *device_uid, uint8_t endpoint)
{
    field_entry_t *entry = find_field(device_uid, endpoint, "onoff");
    if (!entry || entry->kind != FieldKind::Switch || !entry->obj) {
        return;
    }
    const ui_control_ack_status_t st = ui_control_ack_get_status(device_uid, endpoint, "onoff");
    if (st == UI_CONTROL_ACK_PENDING) {
        lv_obj_add_state(entry->obj, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(entry->obj, LV_STATE_DISABLED);
    }
    if (st == UI_CONTROL_ACK_ERROR) {
        lv_obj_add_state(entry->obj, kStateError);
    } else {
        lv_obj_remove_state(entry->obj, kStateError);
    }
}
