use super::*;

pub(super) unsafe fn hide_menu() {
    MENU_VISIBLE.store(false, Ordering::Relaxed);
    let card = MENU_CARD.load(Ordering::Relaxed);
    if !card.is_null() {
        lv_obj_set_hidden(card, true);
    }
    let backdrop = MENU_BACKDROP.load(Ordering::Relaxed);
    if !backdrop.is_null() {
        lv_obj_set_hidden(backdrop, true);
    }
}

pub(super) unsafe fn show_menu() {
    if MENU_CARD.load(Ordering::Relaxed).is_null() {
        return;
    }
    MENU_VISIBLE.store(true, Ordering::Relaxed);
    let backdrop = MENU_BACKDROP.load(Ordering::Relaxed);
    if !backdrop.is_null() {
        lv_obj_set_hidden(backdrop, false);
    }
    lv_obj_set_hidden(MENU_CARD.load(Ordering::Relaxed), false);
}

pub(super) unsafe fn toggle_menu() {
    if MENU_VISIBLE.load(Ordering::Relaxed) {
        hide_menu();
    } else {
        show_menu();
    }
}

pub(super) unsafe extern "C" fn on_bar_pressed(_event: *mut lv_event_t) {
    let indev = lv_indev_active();
    if indev.is_null() {
        return;
    }
    let mut point = lv_point_t::default();
    lv_indev_get_point(indev, &mut point);
    PRESS_Y.store(point.y, Ordering::Relaxed);
}

pub(super) unsafe extern "C" fn on_bar_released(_event: *mut lv_event_t) {
    let indev = lv_indev_active();
    if indev.is_null() {
        return;
    }
    let mut point = lv_point_t::default();
    lv_indev_get_point(indev, &mut point);
    if point.y - PRESS_Y.load(Ordering::Relaxed) >= 30 {
        toggle_menu();
    }
}

unsafe extern "C" fn on_backdrop(_event: *mut lv_event_t) {
    hide_menu();
}

unsafe extern "C" fn on_menu_item(event: *mut lv_event_t) {
    let nav = lv_event_get_user_data(event) as *const Nav;
    if nav.is_null() {
        return;
    }
    hide_menu();
    NAV.lock().unwrap().push_back(*nav);
}

unsafe fn add_menu_item(menu: *mut lv_obj_t, text: &str, nav: Nav) {
    let button = lv_button_create(menu);
    lv_obj_set_size(button, 184, 56);
    lv_obj_set_style_bg_color(button, color(COL_CHIP), 0);
    lv_obj_set_style_radius(button, 10, 0);
    let label = label(button, text, font_20(), COL_TEXT);
    lv_obj_align(label, lv_align_t_LV_ALIGN_LEFT_MID, 12, 0);
    let ptr = Box::into_raw(Box::new(nav));
    lv_obj_add_event_cb(
        button,
        Some(on_menu_item),
        lv_event_code_t_LV_EVENT_CLICKED,
        ptr as *mut _,
    );
    lv_obj_add_event_cb(
        button,
        Some(free_nav),
        lv_event_code_t_LV_EVENT_DELETE,
        ptr as *mut _,
    );
}

pub(super) unsafe fn build_menu() {
    let layer = lv_layer_top();
    let backdrop = lv_obj_create(layer);
    lv_obj_set_size(backdrop, SCREEN_W, SCREEN_H);
    lv_obj_align(backdrop, lv_align_t_LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(backdrop, 0, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_set_scrollable(backdrop, false);
    lv_obj_add_flag(backdrop, lv_obj_flag_t_LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        backdrop,
        Some(on_backdrop),
        lv_event_code_t_LV_EVENT_CLICKED,
        null_mut(),
    );
    lv_obj_set_hidden(backdrop, true);
    MENU_BACKDROP.store(backdrop, Ordering::Relaxed);

    let menu = lv_obj_create(layer);
    lv_obj_set_size(menu, 200, 136);
    lv_obj_align(menu, lv_align_t_LV_ALIGN_TOP_RIGHT, -12, STATUSBAR_H + 8);
    lv_obj_set_style_bg_color(menu, color(COL_CARD), 0);
    lv_obj_set_style_bg_opa(menu, 255, 0);
    lv_obj_set_style_radius(menu, 16, 0);
    lv_obj_set_style_border_width(menu, 0, 0);
    lv_obj_set_style_pad_all(menu, 8, 0);
    lv_obj_set_style_pad_row(menu, 6, 0);
    lv_obj_set_scrollable(menu, false);
    lv_obj_set_flex_flow(menu, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        menu,
        0,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );
    lv_obj_set_hidden(menu, true);
    MENU_CARD.store(menu, Ordering::Relaxed);

    add_menu_item(menu, "Настройки", Nav::OpenSettings);
    add_menu_item(menu, "Wi-Fi", Nav::OpenWifi);
}

/* --- status bar --------------------------------------------------------- */

static MENU_CARD: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static MENU_BACKDROP: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static MENU_VISIBLE: AtomicBool = AtomicBool::new(false);
static PRESS_Y: AtomicI32 = AtomicI32::new(0);
