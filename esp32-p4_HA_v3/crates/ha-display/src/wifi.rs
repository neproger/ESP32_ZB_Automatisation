//! Wi-Fi page, scan list and connect dialog.

use super::*;

pub(super) unsafe fn close_wifi_dialog() {
    let keyboard = WIFI_KEYBOARD.swap(null_mut(), Ordering::Relaxed);
    if !keyboard.is_null() {
        lv_obj_delete(keyboard);
    }
    let dialog = WIFI_DIALOG.swap(null_mut(), Ordering::Relaxed);
    if !dialog.is_null() {
        lv_obj_delete(dialog);
    }
    let backdrop = WIFI_BACKDROP.swap(null_mut(), Ordering::Relaxed);
    if !backdrop.is_null() {
        lv_obj_delete(backdrop);
    }
    WIFI_TEXTAREA.store(null_mut(), Ordering::Relaxed);
    *DIALOG_SSID.lock().unwrap() = None;
}

unsafe extern "C" fn on_wifi_cancel(_event: *mut lv_event_t) {
    close_wifi_dialog();
}

unsafe extern "C" fn on_wifi_connect(_event: *mut lv_event_t) {
    let textarea = WIFI_TEXTAREA.load(Ordering::Relaxed);
    let password = if textarea.is_null() {
        String::new()
    } else {
        let text = lv_textarea_get_text(textarea);
        if text.is_null() {
            String::new()
        } else {
            CStr::from_ptr(text).to_string_lossy().into_owned()
        }
    };
    if let Some(ssid) = DIALOG_SSID.lock().unwrap().clone() {
        if let Err(error) = post_wifi_connect(&ssid, &password) {
            log::warn!("display: wifi connect rejected: {error:?}");
        }
    }
    close_wifi_dialog();
}

struct ApCtx {
    ssid: String,
}

unsafe extern "C" fn on_ap(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *const ApCtx;
    if context.is_null() {
        return;
    }
    open_wifi_dialog(&(*context).ssid);
}

unsafe extern "C" fn free_ap_context(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *mut ApCtx;
    if !context.is_null() {
        drop(Box::from_raw(context));
    }
}

pub(super) unsafe fn open_wifi_dialog(ssid: &str) {
    close_wifi_dialog();
    *DIALOG_SSID.lock().unwrap() = Some(ssid.to_string());

    let layer = lv_layer_top();
    let backdrop = lv_obj_create(layer);
    lv_obj_set_size(backdrop, SCREEN_W, SCREEN_H);
    lv_obj_align(backdrop, lv_align_t_LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(backdrop, color(0x000000), 0);
    lv_obj_set_style_bg_opa(backdrop, 153, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_set_scrollable(backdrop, false);
    WIFI_BACKDROP.store(backdrop, Ordering::Relaxed);

    let dialog = lv_obj_create(backdrop);
    lv_obj_set_size(dialog, 440, 220);
    lv_obj_align(dialog, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 20);
    lv_obj_set_style_bg_color(dialog, color(COL_CARD), 0);
    lv_obj_set_style_bg_opa(dialog, 255, 0);
    lv_obj_set_style_radius(dialog, 16, 0);
    lv_obj_set_style_border_width(dialog, 0, 0);
    lv_obj_set_style_pad_all(dialog, 16, 0);
    lv_obj_set_style_pad_row(dialog, 12, 0);
    lv_obj_set_flex_flow(dialog, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        dialog,
        0,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_START,
    );
    label(dialog, &format!("Пароль: {ssid}"), font_20(), COL_TEXT);

    let textarea = lv_textarea_create(dialog);
    lv_obj_set_size(textarea, 400, 52);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_password_mode(textarea, true);
    if let Ok(hint) = CString::new("Пароль") {
        lv_textarea_set_placeholder_text(textarea, hint.as_ptr());
    }
    lv_obj_set_style_text_font(textarea, font_20(), 0);

    let row = transparent_box(dialog, 400, 56);
    lv_obj_set_flex_flow(row, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_align(
        row,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );

    let cancel = lv_button_create(row);
    lv_obj_set_size(cancel, 180, 52);
    lv_obj_set_style_bg_color(cancel, color(COL_CHIP), 0);
    lv_obj_set_style_radius(cancel, 10, 0);
    let cancel_label = label(cancel, "Отмена", font_20(), COL_TEXT);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(
        cancel,
        Some(on_wifi_cancel),
        lv_event_code_t_LV_EVENT_CLICKED,
        null_mut(),
    );

    let connect = lv_button_create(row);
    lv_obj_set_size(connect, 180, 52);
    lv_obj_set_style_bg_color(connect, color(COL_ACCENT), 0);
    lv_obj_set_style_radius(connect, 10, 0);
    let connect_label = label(connect, "Подключить", font_20(), COL_TEXT);
    lv_obj_center(connect_label);
    lv_obj_add_event_cb(
        connect,
        Some(on_wifi_connect),
        lv_event_code_t_LV_EVENT_CLICKED,
        null_mut(),
    );

    let keyboard = lv_keyboard_create(layer);
    lv_obj_set_size(keyboard, SCREEN_W, 240);
    lv_obj_align(keyboard, lv_align_t_LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, textarea);

    WIFI_TEXTAREA.store(textarea, Ordering::Relaxed);
    WIFI_DIALOG.store(dialog, Ordering::Relaxed);
    WIFI_KEYBOARD.store(keyboard, Ordering::Relaxed);
}

pub(super) fn wifi_status(domain: &DomainHandle) -> Option<WifiStatusRecord> {
    let mut bytes = [0u8; WIFI_STATUS_RECORD_LEN];
    domain
        .entity_get(
            ENTITY_WIFI_STATUS,
            &WIFI_DEVICE_UID.to_le_bytes(),
            &mut bytes,
        )
        .ok()
        .and_then(|_| WifiStatusRecord::decode(&bytes))
}

pub(super) fn wifi_status_text(domain: &DomainHandle) -> (String, bool) {
    match wifi_status(domain) {
        Some(record) if record.connected != 0 => (
            format!("{}  {} dBm", c_string(&record.ssid), record.rssi),
            true,
        ),
        _ => ("не подключено".to_string(), false),
    }
}

pub(super) unsafe fn fill_wifi_list(domain: &DomainHandle, list: *mut lv_obj_t) {
    let mut bytes = [0u8; WIFI_SCAN_RECORD_LEN];
    let networks: Vec<(String, i8)> = domain
        .entity_get(ENTITY_WIFI_SCAN, &WIFI_DEVICE_UID.to_le_bytes(), &mut bytes)
        .ok()
        .and_then(|_| WifiScanRecord::decode(&bytes))
        .map(|record| {
            let count = usize::from(record.count).min(record.aps.len());
            record.aps[..count]
                .iter()
                .filter(|ap| ap.ssid[0] != 0)
                .map(|ap| (c_string(&ap.ssid), ap.rssi))
                .collect()
        })
        .unwrap_or_default();
    if networks.is_empty() {
        label(list, "Нажмите «Сканировать»", font_20(), COL_MUTED);
        return;
    }
    for (ssid, rssi) in networks {
        let button = lv_button_create(list);
        lv_obj_set_size(button, 430, 56);
        lv_obj_set_style_bg_color(button, color(COL_CHIP), 0);
        lv_obj_set_style_radius(button, 10, 0);
        let name = label(button, &ssid, font_20(), COL_TEXT);
        lv_obj_align(name, lv_align_t_LV_ALIGN_LEFT_MID, 12, 0);
        let signal = label(button, &format!("{rssi} dBm"), font_20(), COL_MUTED);
        lv_obj_align(signal, lv_align_t_LV_ALIGN_RIGHT_MID, -12, 0);
        let context = Box::into_raw(Box::new(ApCtx { ssid }));
        lv_obj_add_event_cb(
            button,
            Some(on_ap),
            lv_event_code_t_LV_EVENT_CLICKED,
            context as *mut _,
        );
        lv_obj_add_event_cb(
            button,
            Some(free_ap_context),
            lv_event_code_t_LV_EVENT_DELETE,
            context as *mut _,
        );
    }
}
pub(super) unsafe fn build_wifi(domain: &DomainHandle) -> Page {
    close_wifi_dialog();
    let screen = lv_obj_create(null_mut());
    lv_obj_set_style_bg_color(screen, color(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, 255, 0);
    lv_obj_set_scrollable(screen, false);

    let back = label(screen, ICON_LEFT, font_icons(), COL_TEXT);
    lv_obj_align(back, lv_align_t_LV_ALIGN_TOP_LEFT, 12, STATUSBAR_H + 10);
    nav_target(back, Nav::Back);

    let title = label(screen, "Wi-Fi", font_32(), COL_TEXT);
    lv_obj_align(title, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 10);

    let (status_text, connected) = wifi_status_text(domain);
    let status = label(
        screen,
        &status_text,
        font_20(),
        if connected { COL_OK } else { COL_MUTED },
    );
    lv_obj_align(status, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 60);
    WIFI_STATUS_LABEL.store(status, Ordering::Relaxed);

    let scan = lv_button_create(screen);
    lv_obj_set_size(scan, 180, 52);
    lv_obj_align(scan, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 100);
    lv_obj_set_style_bg_color(scan, color(COL_ACCENT), 0);
    lv_obj_set_style_radius(scan, 10, 0);
    let scan_label = label(scan, "Сканировать", font_20(), COL_TEXT);
    lv_obj_center(scan_label);
    nav_target(scan, Nav::WifiScan);

    let list = transparent_box(screen, 460, 480);
    lv_obj_align(list, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 170);
    lv_obj_set_style_pad_all(list, 10, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_flex_flow(list, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        list,
        0,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_START,
    );

    WIFI_LIST.store(list, Ordering::Relaxed);
    fill_wifi_list(domain, list);

    lv_screen_load(screen);
    Page {
        screen,
        widgets: Vec::new(),
        colors: Vec::new(),
        automations: Vec::new(),
    }
}

pub(super) fn refresh_wifi(domain: &DomainHandle) {
    if VIEW.load(Ordering::Relaxed) != View::Wifi as u8 {
        return;
    }
    let status = WIFI_STATUS_LABEL.load(Ordering::Relaxed);
    let list = WIFI_LIST.load(Ordering::Relaxed);
    if status.is_null() || list.is_null() {
        return;
    }
    let (text, connected) = wifi_status_text(domain);
    if !lock() {
        return;
    }
    unsafe {
        set_label(status, &text);
        lv_obj_set_style_text_color(status, color(if connected { COL_OK } else { COL_MUTED }), 0);
        lv_obj_clean(list);
        fill_wifi_list(domain, list);
    }
    unlock();
}
