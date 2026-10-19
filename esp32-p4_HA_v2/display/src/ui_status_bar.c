#include "ui_status_bar.h"

#include <stdio.h>
#include <string.h>

#include "ha_model/ha_entities.h"
#include "ha_model/ha_entity.h"
#include "ha_model/ha_properties.h"
#include "ha_model/ha_state.h"
#include "ha_model/ha_system.h"
#include "ha_model/ha_weather.h"
#include "ha_model/ha_wifi.h"
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
static lv_obj_t *s_wifi_bars_box;
static lv_obj_t *s_wifi_bars[3];
static lv_obj_t *s_wifi_cross;
static uint32_t s_last_wifi_color;
static int s_last_wifi_level = -1;

static lv_point_t s_press_point;

/* Свайп вниз по строке состояния — меню. Считаем по дельте Y между нажатием и
 * отпусканием: LV_EVENT_GESTURE на верхнем слое (lv_layer_top) не приходит. */
static void on_bar_pressed(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL) {
        lv_indev_get_point(indev, &s_press_point);
    }
}

static void on_bar_released(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (point.y - s_press_point.y >= 30) {
        ui_menu_toggle();
    }
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
    s_last_wifi_color = 0;
    s_last_wifi_level = -1;

    lv_obj_t *layer = lv_layer_top();
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    /* Слой не должен скроллиться: иначе свайп по строке станет скроллом и жест
     * (меню) не придёт. */
    ui_set_scrollable(layer, false);

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

    /* Wi-Fi: уровень/состояние вместо бургера; меню — свайп вниз по строке. */
    lv_obj_t *wifi = lv_obj_create(bar);
    lv_obj_set_size(wifi, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wifi, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi, 0, 0);
    lv_obj_set_style_pad_all(wifi, 0, 0);
    lv_obj_set_style_pad_column(wifi, 6, 0);
    ui_set_scrollable(wifi, false);
    lv_obj_set_flex_flow(wifi, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Уровень сети — три полоски (растут вверх); нет связи — крестик. */
    s_wifi_bars_box = lv_obj_create(wifi);
    lv_obj_set_size(s_wifi_bars_box, 16, 16);
    lv_obj_set_style_bg_opa(s_wifi_bars_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_bars_box, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_bars_box, 0, 0);
    lv_obj_set_style_pad_column(s_wifi_bars_box, 2, 0);
    ui_set_scrollable(s_wifi_bars_box, false);
    lv_obj_set_flex_flow(s_wifi_bars_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wifi_bars_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    for (size_t i = 0; i < 3; i++) {
        lv_obj_t *bar_piece = lv_obj_create(s_wifi_bars_box);
        lv_obj_set_size(bar_piece, 4, 6 + (int)i * 4);
        lv_obj_set_style_bg_opa(bar_piece, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar_piece, 0, 0);
        lv_obj_set_style_radius(bar_piece, 1, 0);
        lv_obj_set_style_pad_all(bar_piece, 0, 0);
        ui_set_scrollable(bar_piece, false);
        s_wifi_bars[i] = bar_piece;
    }

    s_wifi_cross = lv_label_create(wifi);
    lv_obj_set_style_text_font(s_wifi_cross, &ui_icons, 0);
    lv_obj_set_style_text_color(s_wifi_cross, lv_color_hex(UI_COL_MUTED), 0);
    lv_label_set_text(s_wifi_cross, UI_ICON_XMARK);
    ui_set_hidden(s_wifi_cross, true);

    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, on_bar_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(bar, on_bar_released, LV_EVENT_RELEASED, NULL);
}

/* Системное свойство (время) — canonical PROPERTY_STATE(ENTITY_SYSTEM, property). */
static bool read_system_byte(ha_property_id_t property, uint8_t *out)
{
    const ha_state_key_t key = {
        .entity = ha_entity_singleton(HA_ENTITY_ISSUER_SYSTEM),
        .property = property,
    };
    ha_state_record_t record = {0};
    if (!sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &key,
                                  &record))) {
        return false;
    }
    double number = 0.0;
    if (!ha_value_to_double(&record.value, &number)) {
        return false;
    }
    *out = (uint8_t)number;
    return true;
}

static void apply_time_location(void)
{
    char time_text[8] = "--:--";
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (read_system_byte(HA_PROPERTY_SYSTEM_HOUR, &hour) &&
        read_system_byte(HA_PROPERTY_SYSTEM_MINUTE, &minute)) {
        snprintf(time_text, sizeof(time_text), "%02u:%02u", (unsigned)hour, (unsigned)minute);
    }
    if (strcmp(time_text, s_last_time) != 0) {
        lv_label_set_text(s_time_label, time_text);
        snprintf(s_last_time, sizeof(s_last_time), "%s", time_text);
    }

    char location_text[HA_LOCATION_NAME_MAX] = "";
    ha_location_record_t location = {0};
    const ha_device_id_t uid = HA_SYSTEM_DEVICE_ID;
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
    const ha_device_id_t uid = HA_WEATHER_DEVICE_ID;
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

static void apply_wifi(void)
{
    ha_wifi_status_record_t status = {0};
    const ha_device_id_t uid = HA_WIFI_DEVICE_ID;
    const bool present = sys_ok(
        domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_WIFI_STATUS, &uid, &status));

    /* Подключено — уровень (dBm) и цвет по силе; иначе состояние без цифры. */
    /* Уровень 0..3; 0 — связи нет (крестик). */
    const bool connected = present && status.connected;
    int level = 0;
    uint32_t color = UI_COL_MUTED;
    if (connected) {
        level = (status.rssi >= -60) ? 3 : (status.rssi >= -75) ? 2 : 1;
        color = (level == 3) ? UI_COL_OK : (level == 2) ? UI_COL_TEXT : UI_COL_DANGER;
    } else if (present && status.state == (uint8_t)HA_WIFI_STATE_CONNECTING) {
        level = 1; /* идёт подключение — одна полоска акцентом */
        color = UI_COL_ACCENT;
    } else if (present && status.state == (uint8_t)HA_WIFI_STATE_ERROR) {
        color = UI_COL_DANGER;
    }

    if (level != s_last_wifi_level) {
        ui_set_hidden(s_wifi_bars_box, level == 0);
        ui_set_hidden(s_wifi_cross, level != 0);
        for (size_t i = 0; i < 3; i++) {
            ui_set_hidden(s_wifi_bars[i], (int)i >= level);
        }
        s_last_wifi_level = level;
        s_last_wifi_color = 0; /* цвет мог смениться вместе с уровнем */
    }
    if (color != s_last_wifi_color) {
        for (size_t i = 0; i < 3; i++) {
            lv_obj_set_style_bg_color(s_wifi_bars[i], lv_color_hex(color), 0);
        }
        lv_obj_set_style_text_color(s_wifi_cross, lv_color_hex(color), 0);
        s_last_wifi_color = color;
    }
}

void ui_status_bar_apply(void)
{
    if (s_domain == NULL) {
        return;
    }
    apply_time_location();
    apply_weather();
    apply_wifi();
}
