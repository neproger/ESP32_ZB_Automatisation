#include "ui_widgets.h"

#include <stdio.h>

#include "ha_model/ha_zigbee.h"
#include "ui_commands.h"
#include "ui_compat.h"
#include "ui_style.h"

struct ui_widget {
    ui_widget_kind_t kind;
    domain_t *domain;
    ha_zb_state_key_t state;

    lv_obj_t *obj;         /* корневой объект виджета */
    lv_obj_t *slider;      /* LEVEL / COLOR_TEMP */
    lv_obj_t *value_label; /* подпись уровня/значения/индикатора */

    /* Цвет: свотч + hue/saturation (соседние атрибуты кластера Color). */
    lv_obj_t *swatch;
    lv_obj_t *hue_slider;
    lv_obj_t *hue_label;
    lv_obj_t *sat_slider;
    lv_obj_t *sat_label;
    ha_zb_state_key_t hue_key;
    ha_zb_state_key_t sat_key;
    uint8_t cur_hue;
    uint8_t cur_sat;
};

ui_widget_kind_t ui_widget_kind_for(uint16_t cluster_id, uint16_t attr_id)
{
    if (cluster_id == HA_ZB_CLUSTER_ON_OFF && attr_id == HA_ZB_ATTR_ON_OFF_ON_OFF) {
        return UI_WIDGET_SWITCH;
    }
    if (cluster_id == HA_ZB_CLUSTER_LEVEL_CONTROL && attr_id == HA_ZB_ATTR_LEVEL_CURRENT_LEVEL) {
        return UI_WIDGET_LEVEL;
    }
    if (cluster_id == HA_ZB_CLUSTER_COLOR_CONTROL) {
        if (attr_id == HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE) {
            return UI_WIDGET_COLOR_TEMP;
        }
        return UI_WIDGET_COLOR;
    }
    if (cluster_id == HA_ZB_CLUSTER_OCCUPANCY_SENSING &&
        attr_id == HA_ZB_ATTR_OCCUPANCY_OCCUPANCY) {
        return UI_WIDGET_INDICATOR;
    }
    if (cluster_id == HA_ZB_CLUSTER_IAS_ZONE && attr_id == HA_ZB_ATTR_IAS_ZONE_ZONE_STATE) {
        return UI_WIDGET_INDICATOR;
    }
    return UI_WIDGET_VALUE;
}

static void format_value(const ha_zb_state_key_t *state, const ha_zb_state_record_t *record,
                         char *out, size_t out_size)
{
    switch (state->cluster_id) {
    case HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT: {
        const int16_t raw = (int16_t)(record->raw & 0xFFFFu);
        snprintf(out, out_size, "%.1f °C", (double)raw / 100.0);
        return;
    }
    case HA_ZB_CLUSTER_RELATIVE_HUMIDITY: {
        const uint16_t raw = (uint16_t)(record->raw & 0xFFFFu);
        snprintf(out, out_size, "%.1f %%", (double)raw / 100.0);
        return;
    }
    case HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT: {
        const uint16_t raw = (uint16_t)(record->raw & 0xFFFFu);
        snprintf(out, out_size, "%u lx", (unsigned)raw);
        return;
    }
    case HA_ZB_CLUSTER_POWER_CONFIG: {
        const uint16_t raw = (uint16_t)(record->raw & 0xFFFFu);
        if (state->attr_id == HA_ZB_ATTR_POWER_CONFIG_BATTERY_VOLTAGE) {
            snprintf(out, out_size, "%.1f V", (double)raw / 10.0);
        } else {
            snprintf(out, out_size, "%u %%", (unsigned)(raw / 2u));
        }
        return;
    }
    default:
        snprintf(out, out_size, "%u", (unsigned)record->raw);
        return;
    }
}

/* --- создание объектов --- */

lv_obj_t *ui_slider_create_styled(lv_obj_t *parent, int min, int max)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_height(slider, 10); /* толщина дорожки; маркер вырастает pad'ом */
    lv_slider_set_range(slider, min, max);

    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_CHIP), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(slider, 0, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
    return slider;
}

static lv_obj_t *make_slider_row(lv_obj_t *parent, int min, int max, lv_obj_t **out_slider,
                                 lv_obj_t **out_label)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 36); /* вмещает круглый маркер (тонкая дорожка + pad) */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    ui_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    *out_slider = ui_slider_create_styled(row, min, max);
    lv_obj_set_flex_grow(*out_slider, 1);

    *out_label = lv_label_create(row);
    lv_obj_set_style_text_font(*out_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(*out_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_width(*out_label, 96);
    lv_obj_set_style_text_align(*out_label, LV_TEXT_ALIGN_RIGHT, 0);
    return row;
}

static lv_obj_t *make_indicator(lv_obj_t *parent, ui_widget_t *widget)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_size(pill, 220, 56);
    lv_obj_set_style_radius(pill, 28, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(UI_COL_CHIP), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    ui_set_scrollable(pill, false);

    widget->value_label = lv_label_create(pill);
    lv_obj_set_style_text_font(widget->value_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(widget->value_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(widget->value_label);
    return pill;
}

static void update_swatch(ui_widget_t *widget)
{
    if (widget->swatch == NULL) {
        return;
    }
    const uint16_t deg = (uint16_t)((widget->cur_hue * 360u + 127u) / 254u);
    const uint8_t pct = (uint8_t)((widget->cur_sat * 100u + 127u) / 254u);
    lv_obj_set_style_bg_color(widget->swatch, lv_color_hsv_to_rgb(deg, pct, 100), 0);
}

static void on_switch_changed(lv_event_t *event)
{
    ui_widget_t *widget = lv_event_get_user_data(event);
    const bool on = lv_obj_has_state(widget->obj, LV_STATE_CHECKED);
    (void)display_send_onoff(&widget->state, on);
}

static void on_slider_released(lv_event_t *event)
{
    ui_widget_t *widget = lv_event_get_user_data(event);
    const int32_t value = lv_slider_get_value(widget->slider);
    if (widget->kind == UI_WIDGET_LEVEL) {
        (void)display_send_level(&widget->state, (uint8_t)value);
    } else if (widget->kind == UI_WIDGET_COLOR_TEMP) {
        (void)display_send_color_temperature(&widget->state, (uint16_t)value);
    }
}

static void on_color_slider(lv_event_t *event)
{
    ui_widget_t *widget = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    if (target == widget->hue_slider) {
        widget->cur_hue = (uint8_t)lv_slider_get_value(widget->hue_slider);
        (void)display_send_hue(&widget->hue_key, widget->cur_hue);
    } else if (target == widget->sat_slider) {
        widget->cur_sat = (uint8_t)lv_slider_get_value(widget->sat_slider);
        (void)display_send_saturation(&widget->sat_key, widget->cur_sat);
    }
    update_swatch(widget);
}

/* Обёртка живёт ровно столько же, сколько её lv_obj: экран удаляется — виджет освобождён. */
static void on_widget_deleted(lv_event_t *event)
{
    lv_free(lv_event_get_user_data(event));
}

static void build_color_widget(lv_obj_t *parent, ui_widget_t *widget)
{
    widget->hue_key = widget->state;
    widget->hue_key.attr_id = HA_ZB_ATTR_COLOR_CURRENT_HUE;
    widget->sat_key = widget->state;
    widget->sat_key.attr_id = HA_ZB_ATTR_COLOR_CURRENT_SATURATION;

    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 12, 0);
    ui_set_scrollable(col, false);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    widget->swatch = lv_obj_create(col);
    lv_obj_set_size(widget->swatch, 200, 90);
    lv_obj_set_style_radius(widget->swatch, 16, 0);
    lv_obj_set_style_border_width(widget->swatch, 0, 0);
    lv_obj_set_style_bg_opa(widget->swatch, LV_OPA_COVER, 0);
    ui_set_scrollable(widget->swatch, false);

    lv_obj_t *hue_row = make_slider_row(col, 0, 254, &widget->hue_slider, &widget->hue_label);
    lv_obj_t *sat_row = make_slider_row(col, 0, 254, &widget->sat_slider, &widget->sat_label);
    lv_obj_set_style_pad_column(hue_row, 12, 0);
    (void)sat_row;

    lv_obj_add_event_cb(widget->hue_slider, on_color_slider, LV_EVENT_VALUE_CHANGED, widget);
    lv_obj_add_event_cb(widget->sat_slider, on_color_slider, LV_EVENT_VALUE_CHANGED, widget);

    widget->obj = col;
    update_swatch(widget);
}

ui_widget_t *ui_widget_create(domain_t *domain, lv_obj_t *parent,
                              const ha_zb_state_key_t *state)
{
    if (parent == NULL || state == NULL) {
        return NULL;
    }

    ui_widget_t *widget = lv_malloc(sizeof(*widget));
    if (widget == NULL) {
        return NULL;
    }
    *widget = (ui_widget_t){0};
    widget->domain = domain;
    widget->state = *state;
    widget->kind = ui_widget_kind_for(state->cluster_id, state->attr_id);

    switch (widget->kind) {
    case UI_WIDGET_SWITCH:
        widget->obj = lv_switch_create(parent);
        lv_obj_set_size(widget->obj, 140, 70);
        lv_obj_add_event_cb(widget->obj, on_switch_changed, LV_EVENT_VALUE_CHANGED, widget);
        break;
    case UI_WIDGET_LEVEL:
        widget->obj = make_slider_row(parent, 0, 254, &widget->slider, &widget->value_label);
        lv_obj_add_event_cb(widget->slider, on_slider_released, LV_EVENT_RELEASED, widget);
        break;
    case UI_WIDGET_COLOR_TEMP:
        widget->obj = make_slider_row(parent, 153, 500, &widget->slider, &widget->value_label);
        lv_obj_add_event_cb(widget->slider, on_slider_released, LV_EVENT_RELEASED, widget);
        break;
    case UI_WIDGET_COLOR:
        build_color_widget(parent, widget);
        break;
    case UI_WIDGET_INDICATOR:
        widget->obj = make_indicator(parent, widget);
        break;
    case UI_WIDGET_VALUE:
    case UI_WIDGET_NONE:
    default:
        widget->kind = UI_WIDGET_VALUE;
        widget->obj = lv_label_create(parent);
        lv_obj_set_style_text_font(widget->obj, UI_FONT_VALUE, 0);
        lv_obj_set_style_text_color(widget->obj, lv_color_hex(UI_COL_TEXT), 0);
        lv_label_set_text(widget->obj, "—");
        widget->value_label = widget->obj;
        break;
    }

    lv_obj_add_event_cb(widget->obj, on_widget_deleted, LV_EVENT_DELETE, widget);
    return widget;
}

static void apply_absent(ui_widget_t *widget)
{
    switch (widget->kind) {
    case UI_WIDGET_SWITCH:
        lv_obj_remove_state(widget->obj, LV_STATE_CHECKED);
        break;
    case UI_WIDGET_LEVEL:
    case UI_WIDGET_COLOR_TEMP:
        lv_slider_set_value(widget->slider, 0, LV_ANIM_OFF);
        lv_label_set_text(widget->value_label, "—");
        break;
    case UI_WIDGET_COLOR:
        lv_obj_set_style_bg_color(widget->swatch, lv_color_hex(UI_COL_CHIP), 0);
        lv_label_set_text(widget->hue_label, "—");
        lv_label_set_text(widget->sat_label, "—");
        break;
    case UI_WIDGET_INDICATOR:
        lv_obj_set_style_bg_color(widget->obj, lv_color_hex(UI_COL_CHIP), 0);
        lv_label_set_text(widget->value_label, "—");
        break;
    default:
        lv_label_set_text(widget->value_label, "—");
        break;
    }
}

static void apply_color(ui_widget_t *widget)
{
    ha_zb_state_record_t hue = {0};
    ha_zb_state_record_t sat = {0};
    const bool has_hue = sys_ok(domain_entity_get(widget->domain, (domain_entity_t)HA_ENTITY_STATE,
                                                  &widget->hue_key, &hue));
    const bool has_sat = sys_ok(domain_entity_get(widget->domain, (domain_entity_t)HA_ENTITY_STATE,
                                                  &widget->sat_key, &sat));
    if (!has_hue && !has_sat) {
        apply_absent(widget);
        return;
    }
    if (has_hue) {
        widget->cur_hue = (uint8_t)(hue.raw & 0xFFu);
        lv_slider_set_value(widget->hue_slider, widget->cur_hue, LV_ANIM_OFF);
        char text[16] = {0};
        snprintf(text, sizeof(text), "%u°", (unsigned)((widget->cur_hue * 360u + 127u) / 254u));
        lv_label_set_text(widget->hue_label, text);
    }
    if (has_sat) {
        widget->cur_sat = (uint8_t)(sat.raw & 0xFFu);
        lv_slider_set_value(widget->sat_slider, widget->cur_sat, LV_ANIM_OFF);
        char text[16] = {0};
        snprintf(text, sizeof(text), "%u %%", (unsigned)((widget->cur_sat * 100u + 127u) / 254u));
        lv_label_set_text(widget->sat_label, text);
    }
    update_swatch(widget);
}

void ui_widget_apply(ui_widget_t *widget, const ha_zb_state_record_t *record, bool present)
{
    if (widget == NULL || widget->obj == NULL) {
        return;
    }
    if (widget->kind == UI_WIDGET_COLOR) {
        apply_color(widget);
        return;
    }
    if (!present || record == NULL) {
        apply_absent(widget);
        return;
    }

    char text[48] = {0};
    switch (widget->kind) {
    case UI_WIDGET_SWITCH:
        if ((record->raw & 0xFFu) != 0) {
            lv_obj_add_state(widget->obj, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(widget->obj, LV_STATE_CHECKED);
        }
        break;
    case UI_WIDGET_LEVEL:
        lv_slider_set_value(widget->slider, (int32_t)(record->raw & 0xFFu), LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%u / 254", (unsigned)(record->raw & 0xFFu));
        lv_label_set_text(widget->value_label, text);
        break;
    case UI_WIDGET_COLOR_TEMP: {
        const unsigned mireds = record->raw & 0xFFFFu;
        lv_slider_set_value(widget->slider, (int32_t)mireds, LV_ANIM_OFF);
        if (mireds > 0) {
            snprintf(text, sizeof(text), "%u K", (unsigned)(1000000u / mireds));
        } else {
            snprintf(text, sizeof(text), "—");
        }
        lv_label_set_text(widget->value_label, text);
        break;
    }
    case UI_WIDGET_INDICATOR: {
        bool active = (record->raw & 0xFFu) != 0;
        const char *label = active ? "Активно" : "Норма";
        uint32_t color = active ? UI_COL_ACCENT : UI_COL_CHIP;
        if (widget->state.cluster_id == HA_ZB_CLUSTER_OCCUPANCY_SENSING) {
            label = active ? "Занято" : "Свободно";
            color = active ? UI_COL_OK : UI_COL_CHIP;
        } else if (widget->state.cluster_id == HA_ZB_CLUSTER_IAS_ZONE) {
            label = active ? "Тревога" : "Норма";
            color = active ? UI_COL_DANGER : UI_COL_CHIP;
        }
        lv_obj_set_style_bg_color(widget->obj, lv_color_hex(color), 0);
        lv_label_set_text(widget->value_label, label);
        break;
    }
    case UI_WIDGET_VALUE:
    case UI_WIDGET_NONE:
    default:
        format_value(&widget->state, record, text, sizeof(text));
        lv_label_set_text(widget->value_label, text);
        break;
    }
}
