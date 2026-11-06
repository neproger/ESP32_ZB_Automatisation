//! Settings page and brightness control.

use super::*;

pub(super) fn display_settings(domain: &DomainHandle) -> SettingsRecord {
    let key = [SETTINGS_ID];
    let mut bytes = [0u8; SETTINGS_RECORD_LEN];
    domain
        .entity_get(ENTITY_SETTINGS, &key, &mut bytes)
        .ok()
        .and_then(|()| SettingsRecord::decode(&bytes))
        .unwrap_or_default()
}

struct BrightnessCtx {
    label: *mut lv_obj_t,
}

unsafe extern "C" fn on_brightness(event: *mut lv_event_t) {
    let target = lv_event_get_target_obj(event);
    let context = lv_event_get_user_data(event) as *const BrightnessCtx;
    if target.is_null() || context.is_null() {
        return;
    }
    let value = lv_slider_get_value(target).clamp(0, 100) as u8;
    set_label((*context).label, &format!("{value} %"));
    ha_display_set_brightness(value);
    if lv_event_get_code(event) == lv_event_code_t_LV_EVENT_RELEASED {
        if let Err(error) = post_settings(value) {
            log::warn!("display: settings rejected: {error:?}");
        }
    }
}

unsafe extern "C" fn free_brightness_context(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *mut BrightnessCtx;
    if !context.is_null() {
        drop(Box::from_raw(context));
    }
}

pub(super) unsafe fn build_settings(domain: &DomainHandle) -> Page {
    let screen = lv_obj_create(null_mut());
    lv_obj_set_style_bg_color(screen, color(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, 255, 0);
    lv_obj_set_scrollable(screen, false);

    let back = label(screen, ICON_LEFT, font_icons(), COL_TEXT);
    lv_obj_align(back, lv_align_t_LV_ALIGN_TOP_LEFT, 12, STATUSBAR_H + 10);
    nav_target(back, Nav::Back);

    let title = label(screen, "Настройки", font_32(), COL_TEXT);
    lv_obj_align(title, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 10);

    let card = transparent_box(screen, 440, 220);
    lv_obj_align(card, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 80);
    lv_obj_set_style_bg_color(card, color(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, 255, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_flow(card, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        card,
        0,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_START,
    );

    let row = transparent_box(card, 400, 64);
    lv_obj_set_flex_flow(row, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        row,
        lv_flex_align_t_LV_FLEX_ALIGN_SPACE_BETWEEN,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );
    label(row, "Подсветка", font_20(), COL_TEXT);

    let settings = display_settings(domain);
    let brightness = settings.brightness_pct.clamp(10, 100);
    let slider = lv_slider_create(row);
    lv_obj_set_size(slider, 190, 16);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, i32::from(brightness), false);
    let value_label = label(row, &format!("{brightness} %"), font_20(), COL_TEXT);

    let context = Box::into_raw(Box::new(BrightnessCtx { label: value_label }));
    lv_obj_add_event_cb(
        slider,
        Some(on_brightness),
        lv_event_code_t_LV_EVENT_VALUE_CHANGED,
        context as *mut _,
    );
    lv_obj_add_event_cb(
        slider,
        Some(on_brightness),
        lv_event_code_t_LV_EVENT_RELEASED,
        context as *mut _,
    );
    lv_obj_add_event_cb(
        slider,
        Some(free_brightness_context),
        lv_event_code_t_LV_EVENT_DELETE,
        context as *mut _,
    );

    lv_screen_load(screen);
    Page {
        screen,
        widgets: Vec::new(),
        colors: Vec::new(),
        automations: Vec::new(),
    }
}

pub(super) fn set_brightness(percent: u8) {
    unsafe { ha_display_set_brightness(percent) };
}
