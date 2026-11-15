use super::*;

pub(super) fn build_status_bar() -> (
    *mut lv_obj_t,
    *mut lv_obj_t,
    *mut lv_obj_t,
    *mut lv_obj_t,
    [*mut lv_obj_t; 3],
    *mut lv_obj_t,
    *mut lv_obj_t,
) {
    unsafe {
        let layer = lv_layer_top();
        lv_obj_set_style_bg_opa(layer, 0, 0);
        lv_obj_set_scrollable(layer, false);

        let bar = lv_obj_create(layer);
        lv_obj_set_size(bar, SCREEN_W, STATUSBAR_H);
        lv_obj_align(bar, lv_align_t_LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(bar, color(COL_CARD), 0);
        lv_obj_set_style_bg_opa(bar, 255, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_pad_left(bar, 20, 0);
        lv_obj_set_style_pad_right(bar, 20, 0);
        lv_obj_set_scrollable(bar, false);
        lv_obj_set_flex_flow(bar, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(
            bar,
            lv_flex_align_t_LV_FLEX_ALIGN_SPACE_BETWEEN,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        );
        lv_obj_add_flag(bar, lv_obj_flag_t_LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            bar,
            Some(on_bar_pressed),
            lv_event_code_t_LV_EVENT_PRESSED,
            null_mut(),
        );
        lv_obj_add_event_cb(
            bar,
            Some(on_bar_released),
            lv_event_code_t_LV_EVENT_RELEASED,
            null_mut(),
        );

        let time = label(bar, "--:--", font_32(), COL_TEXT);

        let location = label(bar, "", font_20(), COL_MUTED);
        lv_obj_set_hidden(location, true);

        let weather = transparent_box(bar, 150, 20);
        lv_obj_set_style_pad_column(weather, 6, 0);
        lv_obj_set_flex_flow(weather, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(
            weather,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        );
        let weather_icon = label(weather, "", font_weather(), COL_TEXT);
        let weather_label = label(weather, "", font_20(), COL_TEXT);
        lv_obj_set_hidden(weather_icon, true);
        lv_obj_set_hidden(weather_label, true);

        let wifi = transparent_box(bar, 44, 16);
        lv_obj_set_style_pad_column(wifi, 6, 0);
        lv_obj_set_flex_flow(wifi, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(
            wifi,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        );

        let bars_box = transparent_box(wifi, 16, 16);
        lv_obj_set_style_pad_column(bars_box, 2, 0);
        lv_obj_set_flex_flow(bars_box, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(
            bars_box,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            lv_flex_align_t_LV_FLEX_ALIGN_END,
            lv_flex_align_t_LV_FLEX_ALIGN_END,
        );

        let mut bars = [std::ptr::null_mut(); 3];
        for (i, bar_ptr) in bars.iter_mut().enumerate() {
            let piece = lv_obj_create(bars_box);
            lv_obj_set_size(piece, 4, 6 + i as i32 * 4);
            lv_obj_set_style_bg_color(piece, color(COL_MUTED), 0);
            lv_obj_set_style_bg_opa(piece, 255, 0);
            lv_obj_set_style_border_width(piece, 0, 0);
            lv_obj_set_style_radius(piece, 1, 0);
            lv_obj_set_style_pad_all(piece, 0, 0);
            lv_obj_set_scrollable(piece, false);
            *bar_ptr = piece;
        }

        let cross = label(wifi, ICON_XMARK, font_icons(), COL_MUTED);
        lv_obj_set_hidden(cross, true);

        (
            time,
            location,
            weather_icon,
            weather_label,
            bars,
            bars_box,
            cross,
        )
    }
}

/// Bring up the panel, touch, LVGL port, status bar and initial Domain view.
pub(super) fn sync_clock(domain: &DomainHandle) {
    let Some(hour) = read_state(domain, ENTITY_SYSTEM, PROPERTY_SYSTEM_HOUR).and_then(|v| numeric(&v))
    else {
        return;
    };
    let Some(minute) = read_state(domain, ENTITY_SYSTEM, PROPERTY_SYSTEM_MINUTE).and_then(|v| numeric(&v))
    else {
        return;
    };
    let clock = (
        hour.round().clamp(0.0, 23.0) as u8,
        minute.round().clamp(0.0, 59.0) as u8,
    );
    let mut last = CLOCK_LAST.lock().unwrap();
    if *last == clock {
        return;
    }
    *last = clock;
    drop(last);
    log::info!("display: clock {:02}:{:02}", clock.0, clock.1);
    set_clock(clock.0, clock.1);
}

pub(super) fn set_clock(hour: u8, minute: u8) {
    let Some(time) = UI.lock().unwrap().as_ref().map(|ui| ui.time) else {
        return;
    };
    if !lock() {
        return;
    }
    set_label(time, &format!("{hour:02}:{minute:02}"));
    unlock();
}

/// Location name (status bar); empty hides the label.
pub(super) fn set_location(name: &str) {
    let Some(location) = UI.lock().unwrap().as_ref().map(|ui| ui.location) else {
        return;
    };
    if !lock() {
        return;
    }
    unsafe { lv_obj_set_hidden(location, name.is_empty()) };
    set_label(location, name);
    unlock();
}

fn weather_glyph(condition: u8) -> &'static str {
    match condition {
        WEATHER_CLEAR => "\u{f00d}",
        WEATHER_CLEAR_NIGHT => "\u{f02e}",
        WEATHER_PARTLYCLOUDY => "\u{f002}",
        WEATHER_CLOUDY | WEATHER_OVERCAST => "\u{f013}",
        WEATHER_FOG => "\u{f014}",
        WEATHER_RAINY => "\u{f019}",
        WEATHER_POURING => "\u{f01a}",
        WEATHER_SNOWY => "\u{f01b}",
        WEATHER_SNOWY_RAINY => "\u{f017}",
        WEATHER_HAIL => "\u{f015}",
        WEATHER_LIGHTNING => "\u{f016}",
        WEATHER_LIGHTNING_RAINY => "\u{f01e}",
        WEATHER_WINDY | WEATHER_WINDY_VARIANT => "\u{f050}",
        _ => "\u{f01e}",
    }
}

/// Current weather in the status bar (V2 `ha_weather_record_t` units).
pub(super) fn set_weather(condition: u8, temperature_c: f32, humidity_pct: f32) {
    let (icon, label) = {
        let guard = UI.lock().unwrap();
        let Some(ui) = guard.as_ref() else {
            return;
        };
        (ui.weather_icon, ui.weather_label)
    };
    if !lock() {
        return;
    }
    unsafe {
        lv_obj_set_hidden(icon, false);
        lv_obj_set_hidden(label, false);
    }
    set_label(icon, weather_glyph(condition));
    set_label(label, &format!("{temperature_c:.1}°  {humidity_pct:.0} %"));
    unlock();
}

/// Wi-Fi level: Some(rssi dBm) when associated, None when there is no link.
pub(super) fn set_wifi(rssi: Option<i16>) {
    let (level, hex) = match rssi {
        Some(rssi) if rssi >= -60 => (3, COL_OK),
        Some(rssi) if rssi >= -75 => (2, COL_TEXT),
        Some(_) => (1, COL_DANGER),
        None => (0, COL_MUTED),
    };
    let (bars, bars_box, cross, old_level, old_color) = {
        let mut guard = UI.lock().unwrap();
        let Some(ui) = guard.as_mut() else {
            return;
        };
        let snapshot = (
            ui.wifi_bars,
            ui.wifi_box,
            ui.wifi_cross,
            ui.wifi_level,
            ui.wifi_color,
        );
        ui.wifi_level = level;
        ui.wifi_color = hex;
        snapshot
    };
    if level == old_level && hex == old_color {
        return;
    }
    if !lock() {
        return;
    }
    unsafe {
        lv_obj_set_hidden(bars_box, level == 0);
        lv_obj_set_hidden(cross, level != 0);
        for (i, bar) in bars.iter().enumerate() {
            lv_obj_set_hidden(*bar, i as i32 >= level);
            lv_obj_set_style_bg_color(*bar, color(hex), 0);
        }
        lv_obj_set_style_text_color(cross, color(hex), 0);
    }
    unlock();
}
