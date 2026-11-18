//! Shared card/row layout for display widgets.
//!
//! A card is a named table: a larger title, then one row per function. A row
//! has a muted caption on the left and a control on the right. Controls are
//! intentionally small primitives; callers keep entity/domain state.

use super::*;

pub(super) unsafe fn widget_card(
    parent: *mut lv_obj_t,
    title: &str,
    height: i32,
) -> (*mut lv_obj_t, *mut lv_obj_t) {
    let card = transparent_box(parent, CARD_W, height);
    lv_obj_set_style_bg_color(card, color(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, 255, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        card,
        0,
        lv_flex_align_t_LV_FLEX_ALIGN_START,
        lv_flex_align_t_LV_FLEX_ALIGN_START,
    );
    let title_label = label(card, &shorten(title, 24), font_32(), COL_TEXT);
    (card, title_label)
}

pub(super) unsafe fn widget_row(parent: *mut lv_obj_t, caption: &str) -> *mut lv_obj_t {
    let row = transparent_box(parent, 400, 46);
    lv_obj_set_flex_flow(row, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        row,
        lv_flex_align_t_LV_FLEX_ALIGN_SPACE_BETWEEN,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );
    label(row, caption, font_20(), COL_MUTED);
    row
}

pub(super) unsafe fn switch_control(parent: *mut lv_obj_t, checked: bool) -> *mut lv_obj_t {
    let control = lv_switch_create(parent);
    lv_obj_set_size(control, 80, 40);
    if checked {
        lv_obj_add_state(control, lv_state_t_LV_STATE_CHECKED);
    }
    control
}

pub(super) unsafe fn slider_control(
    parent: *mut lv_obj_t,
    width: i32,
    min: i32,
    max: i32,
    value: i32,
    text: &str,
) -> (*mut lv_obj_t, *mut lv_obj_t) {
    let group = transparent_box(parent, 230, 46);
    lv_obj_set_flex_flow(group, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        group,
        lv_flex_align_t_LV_FLEX_ALIGN_END,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );
    lv_obj_set_style_pad_column(group, 8, 0);
    let slider = lv_slider_create(group);
    lv_obj_set_size(slider, width, 16);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value.clamp(min, max), false);
    let value_label = label(group, text, font_20(), COL_TEXT);
    (slider, value_label)
}

pub(super) unsafe fn value_control(parent: *mut lv_obj_t, text: &str) -> *mut lv_obj_t {
    label(parent, text, font_32(), COL_TEXT)
}
