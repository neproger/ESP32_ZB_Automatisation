#include "ui_status_bar.h"

#include <stdio.h>
#include <string.h>

#include "ha_model/ha_entities.h"
#include "ha_model/ha_system.h"
#include "ha_model/ha_weather.h"
#include "ui_compat.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_style.h"
#include "wicons.h"

static domain_t *s_domain;
static lv_obj_t *s_time_label;
static lv_obj_t *s_location_label;
static lv_obj_t *s_weather_icon;
static lv_obj_t *s_weather_label;
static char s_last_time[8];
static char s_last_location[HA_LOCATION_NAME_MAX];
static char s_last_weather[40];
static char s_last_glyph[8];

static void on_burger(lv_event_t *event)
{
    (void)event;
    ui_menu_toggle();
}

void ui_status_bar_create(domain_t *domain)
{
    s_domain = domain;
    /* Кэши «последних значений» привязаны к прежним меткам. При пересборке UI
     * (напр. смена темы) метки создаются заново — сбрасываем кэш, иначе apply
     * посчитает «не изменилось» и новые метки останутся пустыми. */
    s_last_time[0] = '\0';
    s_last_location[0] = '\0';
    s_last_weather[0] = '\0';
    s_last_glyph[0] = '\0';

    lv_obj_t *layer = lv_layer_top();
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);

    lv_obj_t *bar = lv_obj_create(layer);
    lv_obj_set_size(bar, lv_pct(100), UI_STATUSBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_left(bar, 20, 0);
    lv_obj_set_style_pad_right(bar, 20, 0);
    lv_obj_set_style_pad_top(bar, 0, 0);
    lv_obj_set_style_pad_bottom(bar, 0, 0);
    ui_set_scrollable(bar, false);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_time_label = lv_label_create(bar);
    lv_obj_set_style_text_font(s_time_label, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text(s_time_label, "--:--");

    s_location_label = lv_label_create(bar);
    lv_obj_set_style_text_font(s_location_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_location_label, lv_color_hex(UI_COL_MUTED), 0);
    lv_label_set_text(s_location_label, "");

    lv_obj_t *weather = lv_obj_create(bar);
    lv_obj_set_size(weather, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(weather, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather, 0, 0);
    lv_obj_set_style_pad_all(weather, 0, 0);
    lv_obj_set_style_pad_column(weather, 8, 0);
    ui_set_scrollable(weather, false);
    lv_obj_set_flex_flow(weather, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Иконка погоды — глиф шрифта Weather Icons (красится цветом текста). */
    s_weather_icon = lv_label_create(weather);
    lv_obj_set_style_text_font(s_weather_icon, &wicons, 0);
    lv_obj_set_style_text_color(s_weather_icon, lv_color_hex(UI_COL_TEXT), 0);

    s_weather_label = lv_label_create(weather);
    lv_obj_set_style_text_font(s_weather_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_weather_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text(s_weather_label, "");

    /* Бургер — меню поверх экрана (ui_menu). */
    lv_obj_t *burger = lv_button_create(bar);
    lv_obj_set_size(burger, 44, 40);
    lv_obj_set_style_bg_opa(burger, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(burger, 0, 0);
    lv_obj_set_style_shadow_width(burger, 0, 0);
    lv_obj_set_style_pad_all(burger, 0, 0);
    lv_obj_add_event_cb(burger, on_burger, LV_EVENT_CLICKED, NULL);
    lv_obj_t *burger_icon = lv_label_create(burger);
    lv_obj_set_style_text_font(burger_icon, &ui_icons, 0);
    lv_obj_set_style_text_color(burger_icon, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text(burger_icon, UI_ICON_BARS);
    lv_obj_center(burger_icon);
}

static bool read_u8(uint16_t attr_id, uint8_t *out)
{
    const ha_zb_state_key_t key = {
        .device_uid = HA_SYSTEM_DEVICE_UID,
        .cluster_id = HA_CLUSTER_SYSTEM,
        .attr_id = attr_id,
        .endpoint = HA_SYSTEM_ENDPOINT,
    };
    ha_zb_state_record_t record = {0};
    if (!sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_STATE, &key, &record))) {
        return false;
    }
    *out = (uint8_t)(record.raw & 0xFFu);
    return true;
}

static void apply_time_location(void)
{
    char time_text[8] = "--:--";
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (read_u8(HA_SYS_ATTR_HOUR, &hour) && read_u8(HA_SYS_ATTR_MINUTE, &minute)) {
        snprintf(time_text, sizeof(time_text), "%02u:%02u", (unsigned)hour, (unsigned)minute);
    }
    if (strcmp(time_text, s_last_time) != 0) {
        lv_label_set_text(s_time_label, time_text);
        snprintf(s_last_time, sizeof(s_last_time), "%s", time_text);
    }

    char location_text[HA_LOCATION_NAME_MAX] = "";
    ha_location_record_t location = {0};
    const ha_device_uid_t uid = HA_SYSTEM_DEVICE_UID;
    if (sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_LOCATION, &uid, &location))) {
        snprintf(location_text, sizeof(location_text), "%s", location.name);
    }
    if (strcmp(location_text, s_last_location) != 0) {
        lv_label_set_text(s_location_label, location_text);
        snprintf(s_last_location, sizeof(s_last_location), "%s", location_text);
        ui_set_hidden(s_location_label, location_text[0] == '\0'); /* нет данных — не рисуем */
    }
}

/* condition -> глиф Weather Icons (см. weather_icons.txt). */
static const char *weather_glyph(uint8_t condition)
{
    switch (condition) {
    case HA_WEATHER_CLEAR:
        return WEATHER_ICON_DAY_SUNNY;
    case HA_WEATHER_CLEAR_NIGHT:
        return WEATHER_ICON_NIGHT_CLEAR;
    case HA_WEATHER_PARTLYCLOUDY:
        return WEATHER_ICON_DAY_CLOUDY;
    case HA_WEATHER_CLOUDY:
    case HA_WEATHER_OVERCAST:
        return WEATHER_ICON_CLOUDY;
    case HA_WEATHER_FOG:
        return WEATHER_ICON_FOG;
    case HA_WEATHER_RAINY:
        return WEATHER_ICON_RAIN;
    case HA_WEATHER_POURING:
        return WEATHER_ICON_SHOWERS;
    case HA_WEATHER_SNOWY:
        return WEATHER_ICON_SNOW;
    case HA_WEATHER_SNOWY_RAINY:
        return WEATHER_ICON_RAIN_MIX;
    case HA_WEATHER_HAIL:
        return WEATHER_ICON_HAIL;
    case HA_WEATHER_LIGHTNING:
        return WEATHER_ICON_LIGHTNING;
    case HA_WEATHER_LIGHTNING_RAINY:
        return WEATHER_ICON_THUNDERSTORM;
    case HA_WEATHER_WINDY:
    case HA_WEATHER_WINDY_VARIANT:
        return WEATHER_ICON_STRONG_WIND;
    default:
        return WEATHER_ICON_THUNDERSTORM;
    }
}

static void apply_weather(void)
{
    ha_weather_record_t weather = {0};
    const ha_device_uid_t uid = HA_WEATHER_DEVICE_UID;
    const bool present =
        sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_WEATHER, &uid, &weather));

    const char *glyph = present ? weather_glyph(weather.condition) : "";
    char text[40] = "";
    if (present) {
        snprintf(text, sizeof(text), "%.1f°  %u %%", (double)weather.temperature_c100 / 100.0,
                 (unsigned)(weather.humidity_p100 / 100u));
    }

    if (strcmp(glyph, s_last_glyph) != 0) {
        lv_label_set_text(s_weather_icon, glyph);
        snprintf(s_last_glyph, sizeof(s_last_glyph), "%s", glyph);
    }
    if (strcmp(text, s_last_weather) != 0) {
        lv_label_set_text(s_weather_label, text);
        snprintf(s_last_weather, sizeof(s_last_weather), "%s", text);
        ui_set_hidden(s_weather_label, !present); /* нет данных — не рисуем */
    }
    ui_set_hidden(s_weather_icon, !present);
}

void ui_status_bar_apply(void)
{
    if (s_domain == NULL) {
        return;
    }
    apply_time_location();
    apply_weather();
}
