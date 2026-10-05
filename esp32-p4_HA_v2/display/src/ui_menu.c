#include "ui_menu.h"

#include <stdint.h>

#include "lvgl.h"
#include "ui_compat.h"
#include "ui_style.h"

static lv_obj_t *s_backdrop;
static lv_obj_t *s_menu;
static ui_menu_cb s_cb;
static bool s_visible;

static void hide(void)
{
    s_visible = false;
    ui_set_hidden(s_backdrop, true);
    ui_set_hidden(s_menu, true);
}

static void show(void)
{
    s_visible = true;
    ui_set_hidden(s_menu, false);
    ui_set_hidden(s_backdrop, false);
    lv_obj_move_foreground(s_menu);
}

void ui_menu_hide(void)
{
    if (s_menu == NULL) {
        return;
    }
    hide();
}

void ui_menu_toggle(void)
{
    if (s_menu == NULL) {
        return;
    }
    if (s_visible) {
        hide();
    } else {
        show();
    }
}

void ui_menu_set_cb(ui_menu_cb cb)
{
    s_cb = cb;
}

static void on_item(lv_event_t *event)
{
    const ui_menu_item_t item = (ui_menu_item_t)(intptr_t)lv_event_get_user_data(event);
    ui_menu_hide();
    if (s_cb != NULL) {
        s_cb(item);
    }
}

static void on_backdrop(lv_event_t *event)
{
    (void)event;
    ui_menu_hide();
}

static void add_item(lv_obj_t *menu, const char *text, ui_menu_item_t item)
{
    lv_obj_t *button = lv_button_create(menu);
    lv_obj_set_width(button, lv_pct(100));
    lv_obj_set_height(button, 56);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COL_CHIP), 0);
    lv_obj_add_event_cb(button, on_item, LV_EVENT_CLICKED, (void *)(intptr_t)item);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
}

void ui_menu_create(void)
{
    lv_obj_t *layer = lv_layer_top();

    s_backdrop = lv_obj_create(layer);
    lv_obj_set_size(s_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_align(s_backdrop, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_backdrop, 0, 0);
    lv_obj_set_style_pad_all(s_backdrop, 0, 0);
    lv_obj_add_event_cb(s_backdrop, on_backdrop, LV_EVENT_CLICKED, NULL);

    s_menu = lv_obj_create(layer);
    lv_obj_set_size(s_menu, 200, LV_SIZE_CONTENT);
    lv_obj_align(s_menu, LV_ALIGN_TOP_RIGHT, -12, UI_STATUSBAR_H + 8);
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(UI_COL_CARD), 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_menu, 16, 0);
    lv_obj_set_style_border_width(s_menu, 0, 0);
    lv_obj_set_style_pad_all(s_menu, 8, 0);
    lv_obj_set_style_pad_row(s_menu, 6, 0);
    ui_set_scrollable(s_menu, false);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_menu, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    add_item(s_menu, "Настройки", UI_MENU_SETTINGS);
    add_item(s_menu, "Wi-Fi", UI_MENU_WIFI);

    hide();
}
