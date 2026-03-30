#include "widget_endpoint_controls.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "ui_actions.hpp"
#include "ui_style.hpp"

namespace
{
constexpr lv_state_t kStateError = LV_STATE_USER_1;
constexpr lv_style_selector_t kSelMainError =
    static_cast<lv_style_selector_t>(static_cast<uint32_t>(LV_PART_MAIN) |
                                     static_cast<uint32_t>(kStateError));
lv_grad_dsc_t s_hue_grad = {};
bool s_hue_grad_ready = false;

void set_value_1dp(lv_obj_t *label, const char *prefix, float v, const char *suffix)
{
    if (!label || !prefix || !suffix)
    {
        return;
    }

    const float scaled_f = v * 10.0f;
    const int32_t scaled = (scaled_f >= 0.0f) ? (int32_t)(scaled_f + 0.5f) : (int32_t)(scaled_f - 0.5f);
    int32_t abs_scaled = (scaled < 0) ? -scaled : scaled;
    const int32_t whole = abs_scaled / 10;
    const int32_t frac = abs_scaled % 10;
    lv_label_set_text_fmt(label, "%s%s%ld.%ld%s", prefix, (scaled < 0) ? "-" : "", (long)whole, (long)frac, suffix);
}

void ensure_hue_gradient()
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
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(0x111827), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 2, LV_PART_KNOB);
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

    if (r < 0.0f) r = 0.0f;
    if (g < 0.0f) g = 0.0f;
    if (b < 0.0f) b = 0.0f;

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

    return lv_color_make((uint8_t)(r * 255.0f + 0.5f),
                         (uint8_t)(g * 255.0f + 0.5f),
                         (uint8_t)(b * 255.0f + 0.5f));
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
} // namespace

WidgetOnOffControl::WidgetOnOffControl(lv_obj_t *parent, const WidgetEndpointRef &ref)
    : WidgetEndpointLeaf(ref)
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_width(root_, lv_pct(100));
    lv_obj_set_height(root_, 56);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root_, on_row_clicked, LV_EVENT_SHORT_CLICKED, this);

    label_ = lv_label_create(root_);
    lv_label_set_text(label_, "On/Off");

    switch_ = lv_switch_create(root_);
    lv_obj_set_size(switch_, 84, 46);
    lv_obj_set_style_border_width(switch_, 2, kSelMainError);
    lv_obj_set_style_border_color(switch_, lv_palette_main(LV_PALETTE_RED), kSelMainError);
    lv_obj_set_style_bg_color(switch_, lv_palette_lighten(LV_PALETTE_RED, 4), kSelMainError);
    lv_obj_clear_flag(switch_, LV_OBJ_FLAG_CLICKABLE);

    render();
}

WidgetOnOffControl::~WidgetOnOffControl()
{
    if (root_) {
        lv_obj_delete(root_);
    }
}

lv_obj_t *WidgetOnOffControl::root() const
{
    return root_;
}

void WidgetOnOffControl::render()
{
    last_version_ = 0;
}

void WidgetOnOffControl::render_if_needed()
{
    const ui_control_ack_status_t st = ui_control_ack_get_status(ref_.uid.uid, ref_.endpoint, "onoff");
    if (st == UI_CONTROL_ACK_PENDING) {
        lv_obj_add_state(switch_, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(switch_, LV_STATE_DISABLED);
    }
}

void WidgetOnOffControl::apply(const WidgetEndpointState &state)
{
    if (state.onoff_version == last_version_) {
        render_if_needed();
        return;
    }
    apply_state(state.has_onoff, state.onoff, state.onoff_version);
}

void WidgetOnOffControl::on_row_clicked(lv_event_t *event)
{
    WidgetOnOffControl *self = static_cast<WidgetOnOffControl *>(lv_event_get_user_data(event));
    if (!self) {
        return;
    }
    self->handle_click();
}

void WidgetOnOffControl::handle_click()
{
    if (ui_control_ack_is_pending(ref_.uid.uid, ref_.endpoint, "onoff")) {
        return;
    }

    bool has_confirmed = false;
    bool confirmed = false;
    (void)ui_control_ack_get_confirmed_bool(ref_.uid.uid, ref_.endpoint, "onoff", &has_confirmed, &confirmed);
    const bool target = has_confirmed ? !confirmed : true;

    if (!ui_control_ack_begin(ref_.uid.uid, ref_.endpoint, "onoff")) {
        return;
    }

    lv_obj_remove_state(switch_, kStateError);
    lv_obj_add_state(switch_, LV_STATE_DISABLED);

    const esp_err_t err = ui_actions_enqueue_onoff(&ref_.uid, ref_.endpoint, target);
    if (err != ESP_OK) {
        ui_control_ack_fail(ref_.uid.uid, ref_.endpoint, "onoff");
        lv_obj_remove_state(switch_, LV_STATE_DISABLED);
        lv_obj_add_state(switch_, kStateError);
    }
}

void WidgetOnOffControl::apply_state(bool has_value, bool value, uint32_t version)
{
    ui_control_ack_confirm_bool(ref_.uid.uid, ref_.endpoint, "onoff", has_value, has_value && value);
    const ui_control_ack_status_t st = ui_control_ack_get_status(ref_.uid.uid, ref_.endpoint, "onoff");

    if (st == UI_CONTROL_ACK_PENDING) {
        lv_obj_add_state(switch_, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(switch_, LV_STATE_DISABLED);
    }
    if (st == UI_CONTROL_ACK_ERROR) {
        lv_obj_add_state(switch_, kStateError);
    } else {
        lv_obj_remove_state(switch_, kStateError);
    }
    if (has_value && value) {
        lv_obj_add_state(switch_, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(switch_, LV_STATE_CHECKED);
    }

    last_version_ = version;
}

WidgetLevelControl::WidgetLevelControl(lv_obj_t *parent, const WidgetEndpointRef &ref)
    : WidgetEndpointLeaf(ref)
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_width(root_, lv_pct(100));
    lv_obj_set_height(root_, LV_SIZE_CONTENT);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    label_ = lv_label_create(root_);
    lv_label_set_text(label_, "Level: 0");

    slider_ = lv_slider_create(root_);
    lv_obj_set_width(slider_, lv_pct(100));
    lv_slider_set_range(slider_, ui_style::kLevelMin, ui_style::kLevelMax);
    lv_obj_add_event_cb(slider_, on_slider_released, LV_EVENT_RELEASED, this);

    render();
}

WidgetLevelControl::~WidgetLevelControl()
{
    if (root_) {
        lv_obj_delete(root_);
    }
}

lv_obj_t *WidgetLevelControl::root() const
{
    return root_;
}

void WidgetLevelControl::render()
{
    last_version_ = 0;
}

void WidgetLevelControl::render_if_needed()
{
}

void WidgetLevelControl::apply(const WidgetEndpointState &state)
{
    if (!state.has_level) {
        if (last_version_ == 0) {
            return;
        }
        lv_slider_set_value(slider_, 0, LV_ANIM_OFF);
        lv_label_set_text(label_, "Level: 0");
        last_version_ = 0;
        return;
    }
    if (state.level_version == last_version_) {
        return;
    }
    lv_slider_set_value(slider_, (int32_t)state.level, LV_ANIM_OFF);
    lv_label_set_text_fmt(label_, "Level: %u", (unsigned)state.level);
    last_version_ = state.level_version;
}

void WidgetLevelControl::on_slider_released(lv_event_t *event)
{
    WidgetLevelControl *self = static_cast<WidgetLevelControl *>(lv_event_get_user_data(event));
    if (!self) {
        return;
    }
    self->handle_release();
}

void WidgetLevelControl::handle_release()
{
    const int32_t value = lv_slider_get_value(slider_);
    (void)ui_actions_enqueue_level(&ref_.uid, ref_.endpoint, (uint8_t)value);
}

WidgetColorControl::WidgetColorControl(lv_obj_t *parent, const WidgetEndpointRef &ref)
    : WidgetEndpointLeaf(ref)
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_width(root_, lv_pct(100));
    lv_obj_set_height(root_, LV_SIZE_CONTENT);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    label_hue_ = lv_label_create(root_);
    lv_label_set_text(label_hue_, "Hue: 0");
    slider_hue_ = lv_slider_create(root_);
    lv_obj_set_width(slider_hue_, lv_pct(100));
    lv_slider_set_range(slider_hue_, 0, 359);
    apply_hue_slider_style(slider_hue_);

    label_sat_ = lv_label_create(root_);
    lv_label_set_text(label_sat_, "Saturation: 100 %");
    slider_sat_ = lv_slider_create(root_);
    lv_obj_set_width(slider_sat_, lv_pct(100));
    lv_slider_set_range(slider_sat_, 0, 100);
    lv_slider_set_value(slider_sat_, 100, LV_ANIM_OFF);

    lv_obj_add_event_cb(slider_hue_, on_hs_released, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(slider_sat_, on_hs_released, LV_EVENT_RELEASED, this);

    render();
}

WidgetColorControl::~WidgetColorControl()
{
    if (root_) {
        lv_obj_delete(root_);
    }
}

lv_obj_t *WidgetColorControl::root() const
{
    return root_;
}

void WidgetColorControl::render()
{
    last_version_ = 0;
    has_cached_hs_ = false;
}

void WidgetColorControl::render_if_needed()
{
}

void WidgetColorControl::apply(const WidgetEndpointState &state)
{
    if (!state.has_color_x || !state.has_color_y) {
        if (last_version_ == 0 && !has_cached_hs_) {
            return;
        }
        last_version_ = 0;
        has_cached_hs_ = false;
        lv_label_set_text(label_hue_, "Hue: 0");
        lv_label_set_text(label_sat_, "Saturation: 100 %");
        lv_slider_set_value(slider_hue_, 0, LV_ANIM_OFF);
        lv_slider_set_value(slider_sat_, 100, LV_ANIM_OFF);
        return;
    }

    const uint32_t version = (state.color_x_version > state.color_y_version) ? state.color_x_version : state.color_y_version;
    if (version == last_version_) {
        return;
    }
    apply_color(state.color_x, state.color_y, version, true);
}

void WidgetColorControl::on_hs_released(lv_event_t *event)
{
    WidgetColorControl *self = static_cast<WidgetColorControl *>(lv_event_get_user_data(event));
    if (!self) {
        return;
    }
    self->handle_release();
}

void WidgetColorControl::handle_release()
{
    const int32_t hue = lv_slider_get_value(slider_hue_);
    const int32_t sat = lv_slider_get_value(slider_sat_);
    if (hue < 0 || sat < 0) {
        return;
    }
    uint16_t x = 0;
    uint16_t y = 0;
    xy_from_hsv((uint16_t)hue, (uint8_t)sat, &x, &y);
    (void)ui_actions_enqueue_color_xy(&ref_.uid, ref_.endpoint, x, y);
}

void WidgetColorControl::apply_color(uint32_t x, uint32_t y, uint32_t version, bool update_sliders)
{
    lv_color_t xy_color = color_from_xy((uint16_t)x, (uint16_t)y);
    lv_color_hsv_t hsv = lv_color_to_hsv(xy_color);
    const bool hue_changed = (!has_cached_hs_) || (cached_hue_ != hsv.h);
    const bool sat_changed = (!has_cached_hs_) || (cached_sat_ != hsv.s);

    if (hue_changed) {
        lv_label_set_text_fmt(label_hue_, "Hue: %u", (unsigned)hsv.h);
    }
    if (sat_changed) {
        lv_label_set_text_fmt(label_sat_, "Saturation: %u %%", (unsigned)hsv.s);
    }
    if (update_sliders) {
        lv_slider_set_value(slider_hue_, (int32_t)hsv.h, LV_ANIM_OFF);
        lv_slider_set_value(slider_sat_, (int32_t)hsv.s, LV_ANIM_OFF);
    }

    cached_hue_ = hsv.h;
    cached_sat_ = hsv.s;
    has_cached_hs_ = true;
    last_version_ = version;
}

WidgetValueLabel::WidgetValueLabel(lv_obj_t *parent, const WidgetEndpointRef &ref, Kind kind)
    : WidgetEndpointLeaf(ref), kind_(kind)
{
    label_ = lv_label_create(parent);
    render();
}

WidgetValueLabel::~WidgetValueLabel()
{
    if (label_) {
        lv_obj_delete(label_);
    }
}

lv_obj_t *WidgetValueLabel::root() const
{
    return label_;
}

void WidgetValueLabel::render()
{
    last_version_ = 0;
}

void WidgetValueLabel::render_if_needed()
{
}

void WidgetValueLabel::apply(const WidgetEndpointState &state)
{
    bool has_value = false;
    uint32_t version = 0;

    if (kind_ == Kind::Temperature) {
        has_value = state.has_temperature_c;
        version = state.temperature_c_version;
    } else if (kind_ == Kind::Humidity) {
        has_value = state.has_humidity_pct;
        version = state.humidity_pct_version;
    } else {
        has_value = state.has_battery_pct;
        version = state.battery_pct_version;
    }

    if (!has_value) {
        if (last_version_ == 0) {
            return;
        }
        if (kind_ == Kind::Battery) lv_label_set_text(label_, "Battery: -");
        else lv_label_set_text(label_, "-");
        last_version_ = 0;
        return;
    }
    if (version == last_version_) {
        return;
    }

    if (kind_ == Kind::Temperature) {
        set_value_1dp(label_, "", state.temperature_c, " C");
    } else if (kind_ == Kind::Humidity) {
        set_value_1dp(label_, "", state.humidity_pct, " %");
    } else {
        lv_label_set_text_fmt(label_, "Battery: %u %%", (unsigned)state.battery_pct);
    }
    last_version_ = version;
}
