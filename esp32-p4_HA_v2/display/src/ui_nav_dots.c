#include "ui_nav_dots.h"

#include "lvgl.h"
#include "ui_compat.h"
#include "ui_style.h"

#define DOTS_MAX 24
#define DOT_SIZE 12

static lv_obj_t *s_bar;
static lv_obj_t *s_dots[DOTS_MAX];
static size_t s_count;

void ui_nav_dots_create(void)
{
    s_count = 0; /* точка отсчёта сброшена: при пересборке UI точки строятся заново */
    lv_obj_t *layer = lv_layer_top();
    s_bar = lv_obj_create(layer);
    lv_obj_set_size(s_bar, LV_SIZE_CONTENT, UI_NAV_DOTS_H);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_set_style_pad_column(s_bar, 10, 0);
    ui_set_scrollable(s_bar, false);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
}

static void rebuild(size_t count)
{
    lv_obj_clean(s_bar);
    s_count = count;
    if (count > DOTS_MAX) {
        count = DOTS_MAX;
    }
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *dot = lv_obj_create(s_bar);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(dot, DOT_SIZE / 2, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(UI_COL_CHIP), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        ui_set_scrollable(dot, false);
        s_dots[i] = dot;
    }
}

void ui_nav_dots_update(size_t count, size_t active)
{
    if (s_bar == NULL) {
        return;
    }
    /* 0/1 экран — точки не нужны. */
    if (count <= 1) {
        if (s_count != 0) {
            lv_obj_clean(s_bar);
            s_count = 0;
        }
        return;
    }
    if (count != s_count) {
        rebuild(count);
    }
    const size_t shown = (count > DOTS_MAX) ? DOTS_MAX : count;
    for (size_t i = 0; i < shown; ++i) {
        const bool on = (i == active);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(on ? UI_COL_ACCENT : UI_COL_CHIP), 0);
        lv_obj_set_style_bg_opa(s_dots[i], on ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}
