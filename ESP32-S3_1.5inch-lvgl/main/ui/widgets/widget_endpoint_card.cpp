#include "widget_endpoint_card.hpp"

#include <stdio.h>
#include <string.h>

#include "gw_core/zb_classify.h"

namespace
{
const char *fallback_title(const WidgetEndpointState &state)
{
    if (state.has_device && state.device.name[0] != '\0') {
        return state.device.name;
    }
    return state.ref.uid.uid;
}

void build_summary_text(const WidgetEndpointState &state, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    if (!state.has_endpoint) {
        strlcpy(out, "Endpoint unavailable", out_size);
        return;
    }

    if (state.caps.temperature && state.has_temperature_c) {
        snprintf(out, out_size, "Temperature %.1f C", (double)state.temperature_c);
        return;
    }
    if (state.caps.humidity && state.has_humidity_pct) {
        snprintf(out, out_size, "Humidity %.1f %%", (double)state.humidity_pct);
        return;
    }
    if (state.caps.onoff && state.has_onoff) {
        strlcpy(out, state.onoff ? "Power on" : "Power off", out_size);
        return;
    }
    if (state.caps.level && state.has_level) {
        snprintf(out, out_size, "Level %u", (unsigned)state.level);
        return;
    }
    if (state.caps.battery && state.has_battery_pct) {
        snprintf(out, out_size, "Battery %u %%", (unsigned)state.battery_pct);
        return;
    }
    strlcpy(out, "No state yet", out_size);
}
} // namespace

WidgetEndpointCard::WidgetEndpointCard(lv_obj_t *parent, const WidgetEndpointRef &ref)
    : ref_(ref)
{
    root_ = lv_obj_create(parent);
    lv_obj_set_width(root_, lv_pct(100));
    lv_obj_set_height(root_, LV_SIZE_CONTENT);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(root_, ui_style::kCardPad, 0);
    lv_obj_set_style_pad_row(root_, ui_style::kCardRowGap, 0);
    lv_obj_set_style_radius(root_, ui_style::kCardRadius, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(ui_style::kCardBgHex), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(root_, lv_color_hex(ui_style::kBorderHex), 0);
    lv_obj_set_style_border_width(root_, 1, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    title_ = lv_label_create(root_);
    lv_obj_set_style_text_font(title_, ui_style::kFontSectionTitle, 0);
    lv_obj_set_style_text_color(title_, lv_color_hex(ui_style::kCardTitleHex), 0);

    subtitle_ = lv_label_create(root_);
    lv_obj_set_style_text_font(subtitle_, ui_style::kFontSubtitle, 0);
    lv_obj_set_style_text_color(subtitle_, lv_color_hex(ui_style::kSubtitleTextHex), 0);

    summary_ = lv_label_create(root_);
    lv_obj_set_style_text_font(summary_, ui_style::kFontBody, 0);
    lv_obj_set_style_text_color(summary_, lv_color_hex(ui_style::kTitleTextHex), 0);

    render();
}

WidgetEndpointCard::~WidgetEndpointCard()
{
    clear_children();
    if (root_) {
        lv_obj_delete(root_);
        root_ = nullptr;
        title_ = nullptr;
        subtitle_ = nullptr;
        summary_ = nullptr;
    }
}

lv_obj_t *WidgetEndpointCard::root() const
{
    return root_;
}

void WidgetEndpointCard::render()
{
    WidgetEndpointState state = {};
    (void)widget_endpoint_state_read(ref_, &state);
    rebuild_if_needed(state);
    update_texts(state);
    last_topology_version_ = state.topology_version;
    last_state_version_ = state.state_version;
}

void WidgetEndpointCard::render_if_needed()
{
    WidgetEndpointState state = {};
    (void)widget_endpoint_state_read(ref_, &state);

    if (state.topology_version != last_topology_version_) {
        rebuild_if_needed(state);
    }
    if (state.topology_version != last_topology_version_ ||
        state.state_version != last_state_version_) {
        update_texts(state);
        last_topology_version_ = state.topology_version;
        last_state_version_ = state.state_version;
    }
}

void WidgetEndpointCard::set_ref(const WidgetEndpointRef &ref)
{
    if (widget_endpoint_ref_equals(ref_, ref)) {
        return;
    }
    ref_ = ref;
    last_topology_version_ = 0;
    last_state_version_ = 0;
    render();
}

const WidgetEndpointRef &WidgetEndpointCard::ref() const
{
    return ref_;
}

void WidgetEndpointCard::rebuild_if_needed(const WidgetEndpointState &state)
{
    if (!title_ || !subtitle_ || !summary_) {
        return;
    }

    clear_children();

    if (!state.has_endpoint) {
        lv_label_set_text(subtitle_, "Endpoint");
        return;
    }

    char subtitle[64] = {0};
    snprintf(subtitle, sizeof(subtitle), "EP %u  %s",
             (unsigned)state.endpoint.endpoint,
             gw_zb_endpoint_kind(&state.endpoint));
    lv_label_set_text(subtitle_, subtitle);

    if (state.caps.onoff) {
        onoff_ = new WidgetOnOffControl(root_, ref_);
    }
    if (state.caps.level) {
        level_ = new WidgetLevelControl(root_, ref_);
    }
    if (state.caps.color) {
        color_ = new WidgetColorControl(root_, ref_);
    }
    if (state.caps.temperature) {
        temperature_ = new WidgetValueLabel(root_, ref_, WidgetValueLabel::Kind::Temperature);
    }
    if (state.caps.humidity) {
        humidity_ = new WidgetValueLabel(root_, ref_, WidgetValueLabel::Kind::Humidity);
    }
    if (state.caps.battery) {
        battery_ = new WidgetValueLabel(root_, ref_, WidgetValueLabel::Kind::Battery);
    }
}

void WidgetEndpointCard::update_texts(const WidgetEndpointState &state)
{
    if (!title_ || !subtitle_ || !summary_) {
        return;
    }

    lv_label_set_text(title_, fallback_title(state));

    if (!state.has_endpoint) {
        lv_label_set_text(subtitle_, "Endpoint");
    }

    char summary[96] = {0};
    build_summary_text(state, summary, sizeof(summary));
    lv_label_set_text(summary_, summary);

    if (onoff_) onoff_->render_if_needed();
    if (level_) level_->render_if_needed();
    if (color_) color_->render_if_needed();
    if (temperature_) temperature_->render_if_needed();
    if (humidity_) humidity_->render_if_needed();
    if (battery_) battery_->render_if_needed();
}

void WidgetEndpointCard::clear_children()
{
    delete onoff_;
    onoff_ = nullptr;
    delete level_;
    level_ = nullptr;
    delete color_;
    color_ = nullptr;
    delete temperature_;
    temperature_ = nullptr;
    delete humidity_;
    humidity_ = nullptr;
    delete battery_;
    battery_ = nullptr;
}
