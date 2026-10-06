#include "ui_settings.h"

#include <stdio.h>
#include <string.h>

#include "ha_model/ha_entities.h"
#include "ha_model/ha_settings.h"
#include "ui_compat.h"
#include "ui_icons.h"
#include "ui_palette.h"
#include "ui_style.h"
#include "ui_widgets.h"

#define UI_SETTINGS_HEADER_H 64

static const uint32_t kTimeoutsMs[] = {0, 30000, 60000, 300000, 600000};
static const char *kTimeoutOptions = "Выкл\n30 секунд\n1 минута\n5 минут\n10 минут";
#define TIMEOUT_COUNT (sizeof(kTimeoutsMs) / sizeof(kTimeoutsMs[0]))

static domain_t *s_domain;
static ui_settings_back_cb s_back;
static ui_settings_theme_cb s_theme;
static lv_obj_t *s_root;
static lv_obj_t *s_brightness;
static lv_obj_t *s_brightness_value;
static lv_obj_t *s_theme_dd;
static lv_obj_t *s_timeout;
static char s_theme_options[48];
static bool s_syncing; /* подавляет запись при программной установке значений */

/* Список имён тем для dropdown («\n»-разделённый). */
static void build_theme_options(void)
{
    s_theme_options[0] = '\0';
    for (int i = 0; i < UI_PALETTE_COUNT; ++i) {
        if (i > 0) {
            strcat(s_theme_options, "\n");
        }
        strcat(s_theme_options, ui_palette_name((ui_palette_id_t)i));
    }
}

static void on_back(lv_event_t *event)
{
    (void)event;
    if (s_back != NULL) {
        s_back();
    }
}

static bool read_settings(ha_settings_record_t *out)
{
    const ha_settings_key_t key = {.id = HA_SETTINGS_ID};
    return sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_SETTINGS, &key, out));
}

static void write_settings(const ha_settings_record_t *record)
{
    const ha_settings_key_t key = {.id = HA_SETTINGS_ID};
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_SETTINGS, &key, record, &meta,
                            &changed);
}

static void on_brightness(lv_event_t *event)
{
    if (s_syncing) {
        return;
    }
    ha_settings_record_t settings = {0};
    if (!read_settings(&settings)) {
        return;
    }
    settings.brightness_pct = (uint8_t)lv_slider_get_value(s_brightness);
    write_settings(&settings);
}

static void on_timeout(lv_event_t *event)
{
    if (s_syncing) {
        return;
    }
    ha_settings_record_t settings = {0};
    if (!read_settings(&settings)) {
        return;
    }
    const uint16_t selected = lv_dropdown_get_selected(s_timeout);
    settings.screensaver_timeout_ms = (selected < TIMEOUT_COUNT) ? kTimeoutsMs[selected] : 0;
    write_settings(&settings);
}

static void on_theme(lv_event_t *event)
{
    if (s_syncing) {
        return;
    }
    ha_settings_record_t settings = {0};
    if (!read_settings(&settings)) {
        return;
    }
    settings.palette_id = (uint8_t)lv_dropdown_get_selected(s_theme_dd);
    write_settings(&settings);
    if (s_theme != NULL) {
        s_theme(); /* владелец экранов пересоберёт UI под новую палитру */
    }
}

static void on_root_deleted(lv_event_t *event)
{
    if (lv_event_get_target(event) != s_root) {
        return; /* удаляется старый экран (anim auto_del) — текущие указатели не трогаем */
    }
    s_root = NULL;
    s_brightness = NULL;
    s_brightness_value = NULL;
    s_theme_dd = NULL;
    s_timeout = NULL;
}

static lv_obj_t *make_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    ui_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t *make_row_label(lv_obj_t *row, const char *text)
{
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_width(label, 150);
    return label;
}

lv_obj_t *ui_settings_create(domain_t *domain, ui_settings_back_cb back,
                             ui_settings_theme_cb theme_changed)
{
    s_domain = domain;
    s_back = back;
    s_theme = theme_changed;

    s_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    ui_set_scrollable(s_root, false);
    lv_obj_add_event_cb(s_root, on_root_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *back_btn = lv_button_create(s_root);
    lv_obj_set_size(back_btn, 56, 44);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 12, UI_STATUSBAR_H + 10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(UI_COL_CHIP), 0);
    lv_obj_add_event_cb(back_btn, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, UI_ICON_ANGLE_LEFT);
    lv_obj_set_style_text_font(back_label, &ui_icons, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "Настройки");
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + 18);

    lv_obj_t *card = lv_obj_create(s_root);
    lv_obj_set_size(card, 440, 300);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + UI_SETTINGS_HEADER_H + 16);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    ui_set_scrollable(card, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Подсветка: подпись + слайдер + значение. */
    lv_obj_t *brightness_row = make_row(card);
    make_row_label(brightness_row, "Подсветка");
    s_brightness = ui_slider_create_styled(brightness_row, 0, 100);
    lv_obj_set_flex_grow(s_brightness, 1);
    lv_obj_add_event_cb(s_brightness, on_brightness, LV_EVENT_RELEASED, NULL);
    s_brightness_value = lv_label_create(brightness_row);
    lv_obj_set_style_text_font(s_brightness_value, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_brightness_value, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_width(s_brightness_value, 64);
    lv_obj_set_style_text_align(s_brightness_value, LV_TEXT_ALIGN_RIGHT, 0);

    /* Тема: подпись + выпадающий список палитр. */
    build_theme_options();
    lv_obj_t *theme_row = make_row(card);
    make_row_label(theme_row, "Тема");
    s_theme_dd = ui_dropdown_create_styled(theme_row);
    lv_obj_set_flex_grow(s_theme_dd, 1);
    lv_dropdown_set_options(s_theme_dd, s_theme_options);
    lv_obj_add_event_cb(s_theme_dd, on_theme, LV_EVENT_VALUE_CHANGED, NULL);

    /* Скринсейвер: подпись + выпадающий список. */
    lv_obj_t *timeout_row = make_row(card);
    make_row_label(timeout_row, "Скринсейвер");
    s_timeout = ui_dropdown_create_styled(timeout_row);
    lv_obj_set_flex_grow(s_timeout, 1);
    lv_dropdown_set_options(s_timeout, kTimeoutOptions);
    lv_obj_add_event_cb(s_timeout, on_timeout, LV_EVENT_VALUE_CHANGED, NULL);

    s_syncing = false;
    return s_root;
}

void ui_settings_apply(void)
{
    if (s_root == NULL || s_brightness == NULL || s_theme_dd == NULL || s_timeout == NULL) {
        return;
    }
    ha_settings_record_t settings = {0};
    if (!read_settings(&settings)) {
        return;
    }
    s_syncing = true;
    if (lv_slider_get_value(s_brightness) != (int32_t)settings.brightness_pct) {
        lv_slider_set_value(s_brightness, settings.brightness_pct, LV_ANIM_OFF);
    }
    char text[16] = {0};
    snprintf(text, sizeof(text), "%u %%", (unsigned)settings.brightness_pct);
    lv_label_set_text(s_brightness_value, text);

    const uint16_t theme_sel =
        (settings.palette_id < UI_PALETTE_COUNT) ? settings.palette_id : UI_PALETTE_RETRO;
    if (lv_dropdown_get_selected(s_theme_dd) != theme_sel) {
        lv_dropdown_set_selected(s_theme_dd, theme_sel);
    }

    uint16_t selected = 0;
    for (uint16_t i = 0; i < TIMEOUT_COUNT; ++i) {
        if (kTimeoutsMs[i] == settings.screensaver_timeout_ms) {
            selected = i;
            break;
        }
    }
    if (lv_dropdown_get_selected(s_timeout) != selected) {
        lv_dropdown_set_selected(s_timeout, selected);
    }
    s_syncing = false;
}
