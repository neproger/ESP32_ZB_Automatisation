//! Display service. Panel/touch/LVGL port live in C (`native_display`); screens,
//! state and all widget calls are driven from Rust through the LVGL bindings.
//!
//! LVGL is not thread safe: the C port task renders, so every access from here is
//! made under `ha_display_lock`.

mod lvgl_bindings {
    #![allow(warnings, clippy::all)]
    include!(concat!(env!("OUT_DIR"), "/lvgl_bindings.rs"));
}
use lvgl_bindings::*;

use std::collections::VecDeque;
use std::ffi::{CStr, CString};
use std::ptr::{addr_of, null_mut};
use std::sync::atomic::{
    AtomicBool, AtomicI32, AtomicPtr, AtomicU32, AtomicU8, AtomicUsize, Ordering,
};
use std::sync::mpsc::Receiver;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use ha_domain::canonical::{
    CanonicalCommand, CapabilitiesRecord, CommandValue, EntityId, EntityRecord, GroupAutomationKey,
    GroupItemKey, GroupItemRecord, GroupKey, GroupRecord, LocationRecord, PropertyId, RuleRecord,
    SettingsRecord, StateKey, WeatherRecord, WifiScanRecord, WifiStatusRecord, ACTION_OFF,
    ACTION_ON, ACTION_SET, AUTOMATION_RECORD_LEN, CAPABILITIES_LEN, COMMAND_PROPERTY,
    COMMAND_WIFI_CONNECT, COMMAND_WIFI_SCAN, ENTITY_AUTOMATION, ENTITY_CAPABILITIES, ENTITY_ENTITY,
    ENTITY_GROUP, ENTITY_GROUP_AUTOMATION, ENTITY_GROUP_ITEM, ENTITY_LOCATION,
    ENTITY_LOGICAL_DEVICE, ENTITY_PROPERTY_STATE, ENTITY_SETTINGS, ENTITY_SYSTEM, ENTITY_WEATHER,
    ENTITY_WIFI_SCAN, ENTITY_WIFI_STATUS, LOCATION_RECORD_LEN, PROPERTY_BATTERY_PERCENT,
    PROPERTY_BATTERY_VOLTAGE, PROPERTY_BRIGHTNESS, PROPERTY_COLOR, PROPERTY_COLOR_TEMPERATURE,
    PROPERTY_COLOR_X, PROPERTY_COLOR_Y, PROPERTY_HUMIDITY, PROPERTY_ILLUMINANCE,
    PROPERTY_OCCUPANCY, PROPERTY_POWER, PROPERTY_SYSTEM_HOUR, PROPERTY_SYSTEM_MINUTE,
    PROPERTY_TEMPERATURE, SETTINGS_ID, SETTINGS_RECORD_LEN, SOURCE_UI, STATE_RECORD_LEN,
    SYSTEM_DEVICE_UID, WEATHER_CLEAR, WEATHER_CLEAR_NIGHT, WEATHER_CLOUDY, WEATHER_DEVICE_UID,
    WEATHER_FOG, WEATHER_HAIL, WEATHER_LIGHTNING, WEATHER_LIGHTNING_RAINY, WEATHER_OVERCAST,
    WEATHER_PARTLYCLOUDY, WEATHER_POURING, WEATHER_RAINY, WEATHER_RECORD_LEN, WEATHER_SNOWY,
    WEATHER_SNOWY_RAINY, WEATHER_WINDY, WEATHER_WINDY_VARIANT, WIFI_DEVICE_UID,
    WIFI_SCAN_RECORD_LEN, WIFI_STATUS_RECORD_LEN,
};
use ha_domain::{DomainHandle, Fact, FactKind, FactMeta, Value};

mod color;
mod model;
mod settings;
mod wifi;
use color::*;
use model::*;
use settings::*;
use wifi::*;

const SCREEN_W: i32 = 480;
const SCREEN_H: i32 = 800;
const STATUSBAR_H: i32 = 56;
const LIST_Y: i32 = STATUSBAR_H + 60;
const LIST_W: i32 = 460;
const LIST_H: i32 = SCREEN_H - LIST_Y - 40;
const CARD_W: i32 = 440;

/* V2 palette roles, Retro preset (ui_palette.c). */
const COL_BG: u32 = 0x000000;
const COL_CARD: u32 = 0x233d4d;
const COL_TEXT: u32 = 0xeaecf0;
const COL_MUTED: u32 = 0x98a2ad;
const COL_ACCENT: u32 = 0xfe7f2d;
const COL_OK: u32 = 0x6fbf73;
const COL_DANGER: u32 = 0xe05440;
const COL_CHIP: u32 = 0x33505f;

const ICON_LEFT: &str = "\u{f104}";
const ICON_RIGHT: &str = "\u{f105}";
const ICON_XMARK: &str = "\u{f00d}";

fn color(hex: u32) -> lv_color_t {
    lv_color_t {
        blue: (hex & 0xff) as u8,
        green: ((hex >> 8) & 0xff) as u8,
        red: ((hex >> 16) & 0xff) as u8,
    }
}

fn font_20() -> *const lv_font_t {
    addr_of!(ui_font_20)
}

fn font_32() -> *const lv_font_t {
    addr_of!(ui_font_32)
}

fn font_icons() -> *const lv_font_t {
    addr_of!(ui_icons)
}

fn font_weather() -> *const lv_font_t {
    addr_of!(wicons)
}

fn lock() -> bool {
    unsafe { ha_display_lock(1000) }
}

fn unlock() {
    unsafe { ha_display_unlock() }
}

fn set_label(obj: *mut lv_obj_t, text: &str) {
    if obj.is_null() {
        return;
    }
    if let Ok(text) = CString::new(text) {
        unsafe { lv_label_set_text(obj, text.as_ptr()) };
    }
}

fn label(parent: *mut lv_obj_t, text: &str, font: *const lv_font_t, hex: u32) -> *mut lv_obj_t {
    unsafe {
        let obj = lv_label_create(parent);
        lv_obj_set_style_text_font(obj, font, 0);
        lv_obj_set_style_text_color(obj, color(hex), 0);
        set_label(obj, text);
        obj
    }
}

fn transparent_box(parent: *mut lv_obj_t, w: i32, h: i32) -> *mut lv_obj_t {
    unsafe {
        let obj = lv_obj_create(parent);
        lv_obj_set_size(obj, w, h);
        lv_obj_set_style_bg_opa(obj, 0, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_set_scrollable(obj, false);
        obj
    }
}

/* --- menu (swipe down on the status bar) -------------------------------- */

unsafe fn hide_menu() {
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

unsafe fn show_menu() {
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

unsafe fn toggle_menu() {
    if MENU_VISIBLE.load(Ordering::Relaxed) {
        hide_menu();
    } else {
        show_menu();
    }
}

unsafe extern "C" fn on_bar_pressed(_event: *mut lv_event_t) {
    let indev = lv_indev_active();
    if indev.is_null() {
        return;
    }
    let mut point = lv_point_t::default();
    lv_indev_get_point(indev, &mut point);
    PRESS_Y.store(point.y, Ordering::Relaxed);
}

unsafe extern "C" fn on_bar_released(_event: *mut lv_event_t) {
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

unsafe fn build_menu() {
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

struct Ui {
    time: *mut lv_obj_t,
    location: *mut lv_obj_t,
    weather_icon: *mut lv_obj_t,
    weather_label: *mut lv_obj_t,
    wifi_bars: [*mut lv_obj_t; 3],
    wifi_box: *mut lv_obj_t,
    wifi_cross: *mut lv_obj_t,
    wifi_level: i32,
    wifi_color: u32,
}

unsafe impl Send for Ui {}

static UI: Mutex<Option<Ui>> = Mutex::new(None);
static DOMAIN: Mutex<Option<DomainHandle>> = Mutex::new(None);
fn domain() -> Result<DomainHandle, ha_domain::Error> {
    DOMAIN
        .lock()
        .unwrap()
        .clone()
        .ok_or(ha_domain::Error::InvalidState)
}

fn post_property(
    entity: EntityId,
    property: PropertyId,
    action: u32,
    value: CommandValue,
) -> Result<(), ha_domain::Error> {
    let command = CanonicalCommand {
        entity,
        property,
        action,
        value,
    };
    domain()?.post(
        COMMAND_PROPERTY,
        &command.encode(),
        None,
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )
}

fn post_settings(brightness: u8) -> Result<(), ha_domain::Error> {
    let domain = domain()?;
    let key = [SETTINGS_ID];
    let mut settings = display_settings(&domain);
    settings.brightness_pct = brightness;
    domain.entity_put(
        ENTITY_SETTINGS,
        &key,
        &settings.encode(),
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )?;
    Ok(())
}

fn post_automation(automation: u64, enabled: bool) -> Result<(), ha_domain::Error> {
    let domain = domain()?;
    let key = automation.to_le_bytes();
    let mut record = [0u8; AUTOMATION_RECORD_LEN];
    domain.entity_get(ENTITY_AUTOMATION, &key, &mut record)?;
    let record =
        ha_automation::set_enabled(&record, enabled).map_err(|_| ha_domain::Error::InvalidState)?;
    domain.entity_put(
        ENTITY_AUTOMATION,
        &key,
        &record,
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )?;
    Ok(())
}

fn post_wifi_connect(ssid: &str, password: &str) -> Result<(), ha_domain::Error> {
    domain()?.post(
        COMMAND_WIFI_CONNECT,
        &ha_wifi::encode_connect(ssid, password),
        None,
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )
}

pub fn request_wifi_scan() -> Result<(), ha_domain::Error> {
    domain()?.post(
        COMMAND_WIFI_SCAN,
        &[],
        None,
        FactMeta {
            source: SOURCE_UI as u8,
            value: Value::NONE,
        },
    )
}

fn build_status_bar() -> (
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
pub fn start(domain: &DomainHandle) -> bool {
    let rc = unsafe { ha_display_start() };
    if rc != 0 {
        log::error!("display: start failed ({rc})");
        return false;
    }
    if !lock() {
        log::error!("display: lock timeout in start");
        return false;
    }
    let (time, location, weather_icon, weather_label, wifi_bars, wifi_box, wifi_cross) =
        build_status_bar();
    *UI.lock().unwrap() = Some(Ui {
        time,
        location,
        weather_icon,
        weather_label,
        wifi_bars,
        wifi_box,
        wifi_cross,
        wifi_level: -1,
        wifi_color: 0,
    });
    unsafe {
        build_menu();
    }
    unlock();
    *DOMAIN.lock().unwrap() = Some(domain.clone());
    STARTED.store(true, Ordering::Relaxed);
    initial_read(domain);
    rebuild(domain);
    log::info!("display: port ready");
    true
}

fn initial_read(domain: &DomainHandle) {
    set_brightness(display_settings(domain).brightness_pct);

    let mut location = [0u8; LOCATION_RECORD_LEN];
    if domain
        .entity_get(
            ENTITY_LOCATION,
            &SYSTEM_DEVICE_UID.to_le_bytes(),
            &mut location,
        )
        .is_ok()
    {
        if let Some(record) = LocationRecord::decode(&location) {
            set_location(&location_text(&record));
        }
    }

    let mut weather = [0u8; WEATHER_RECORD_LEN];
    if domain
        .entity_get(
            ENTITY_WEATHER,
            &WEATHER_DEVICE_UID.to_le_bytes(),
            &mut weather,
        )
        .is_ok()
    {
        if let Some(record) = WeatherRecord::decode(&weather) {
            set_weather(
                record.condition,
                f32::from(record.temperature_c100) / 100.0,
                f32::from(record.humidity_p100) / 100.0,
            );
        }
    }

    if let (Some(hour), Some(minute)) = (
        read_state(domain, ENTITY_SYSTEM, PROPERTY_SYSTEM_HOUR).and_then(|value| numeric(&value)),
        read_state(domain, ENTITY_SYSTEM, PROPERTY_SYSTEM_MINUTE).and_then(|value| numeric(&value)),
    ) {
        let clock = (
            hour.round().clamp(0.0, 23.0) as u8,
            minute.round().clamp(0.0, 59.0) as u8,
        );
        *CLOCK_LAST.lock().unwrap() = clock;
        set_clock(clock.0, clock.1);
    }

    let mut wifi = [0u8; WIFI_STATUS_RECORD_LEN];
    if domain
        .entity_get(
            ENTITY_WIFI_STATUS,
            &WIFI_DEVICE_UID.to_le_bytes(),
            &mut wifi,
        )
        .is_ok()
    {
        if let Some(record) = WifiStatusRecord::decode(&wifi) {
            set_wifi((record.connected != 0).then_some(i16::from(record.rssi)));
        }
    }
}

/// Local clock (status bar), 24-hour.
fn set_clock(hour: u8, minute: u8) {
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
fn set_location(name: &str) {
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
fn set_weather(condition: u8, temperature_c: f32, humidity_pct: f32) {
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
fn set_wifi(rssi: Option<i16>) {
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

/* --- pages -------------------------------------------------------------- */

#[derive(Clone, Copy, PartialEq)]
enum WidgetKind {
    Switch,
    Slider,
    Value,
}

struct WidgetEntry {
    entity: EntityId,
    property: PropertyId,
    kind: WidgetKind,
    control: *mut lv_obj_t,
    value_label: *mut lv_obj_t,
    ctx: *mut CmdCtx,
}

#[derive(Clone, Copy)]
struct ColorEntry {
    entity: EntityId,
    swatch: *mut lv_obj_t,
    hue: *mut lv_obj_t,
    sat: *mut lv_obj_t,
    hue_label: *mut lv_obj_t,
    sat_label: *mut lv_obj_t,
    x: f32,
    y: f32,
}

struct Page {
    screen: *mut lv_obj_t,
    widgets: Vec<WidgetEntry>,
    colors: Vec<ColorEntry>,
    automations: Vec<AutoEntry>,
}

struct AutoEntry {
    automation: u64,
    title: *mut lv_obj_t,
    switch: *mut lv_obj_t,
}

unsafe impl Send for Page {}

#[derive(Clone, Copy)]
pub enum Nav {
    Delta(i32),
    Index(usize),
    OpenSettings,
    OpenWifi,
    Back,
    WifiScan,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum View {
    Page = 0,
    Settings = 1,
    Wifi = 2,
}

static PAGE: Mutex<Option<Page>> = Mutex::new(None);
static GROUPS: Mutex<Vec<GroupRef>> = Mutex::new(Vec::new());
static CURRENT: AtomicUsize = AtomicUsize::new(0);
static NAV: Mutex<VecDeque<Nav>> = Mutex::new(VecDeque::new());
static STARTED: AtomicBool = AtomicBool::new(false);
static VIEW: AtomicU8 = AtomicU8::new(View::Page as u8);
static MENU_CARD: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static MENU_BACKDROP: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static MENU_VISIBLE: AtomicBool = AtomicBool::new(false);
static PRESS_Y: AtomicI32 = AtomicI32::new(0);

static WIFI_DIALOG: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static WIFI_KEYBOARD: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static WIFI_BACKDROP: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static WIFI_TEXTAREA: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static WIFI_STATUS_LABEL: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static WIFI_LIST: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static DIALOG_SSID: Mutex<Option<String>> = Mutex::new(None);
static CLOCK_LAST: Mutex<(u8, u8)> = Mutex::new((0, 0));

const SWITCH_LOCK_MS: u64 = 1500;

struct CmdCtx {
    entity: EntityId,
    property: PropertyId,
    control: *mut lv_obj_t,
    locked: AtomicBool,
    syncing: AtomicBool,
    has_last: AtomicBool,
    last_bits: AtomicU32,
    deadline: Mutex<Option<Instant>>,
}

unsafe extern "C" fn on_control(event: *mut lv_event_t) {
    let target = lv_event_get_target_obj(event);
    let context = lv_event_get_user_data(event) as *const CmdCtx;
    if target.is_null() || context.is_null() {
        return;
    }
    let context = &*context;
    log::debug!(
        "display: control entity={:016x} property={} syncing={} locked={}",
        context.entity,
        context.property,
        context.syncing.load(Ordering::Relaxed),
        context.locked.load(Ordering::Relaxed)
    );
    if context.syncing.load(Ordering::Relaxed) || context.locked.load(Ordering::Relaxed) {
        return;
    }
    let result = if context.property == PROPERTY_POWER {
        let on = lv_obj_has_state(target, lv_state_t_LV_STATE_CHECKED);
        post_property(
            context.entity,
            PROPERTY_POWER,
            if on { ACTION_ON } else { ACTION_OFF },
            CommandValue::None,
        )
    } else {
        let value = lv_slider_get_value(target) as f32;
        post_property(
            context.entity,
            context.property,
            ACTION_SET,
            CommandValue::Scalar(Value::f32(value)),
        )
    };
    if let Err(error) = result {
        log::warn!("display: command rejected: {error:?}");
    }
    if context.property == PROPERTY_POWER {
        context.locked.store(true, Ordering::Relaxed);
        *context.deadline.lock().unwrap() =
            Some(Instant::now() + Duration::from_millis(SWITCH_LOCK_MS));
        lv_obj_add_state(target, lv_state_t_LV_STATE_DISABLED);
    }
}

unsafe extern "C" fn free_context(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *mut CmdCtx;
    if !context.is_null() {
        drop(Box::from_raw(context));
    }
}

struct ColorCtx {
    entity: EntityId,
    hue: *mut lv_obj_t,
    sat: *mut lv_obj_t,
    swatch: *mut lv_obj_t,
    hue_label: *mut lv_obj_t,
    sat_label: *mut lv_obj_t,
}

unsafe extern "C" fn on_color(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *const ColorCtx;
    if context.is_null() {
        return;
    }
    let context = &*context;
    let hue = lv_slider_get_value(context.hue) as f32;
    let sat = lv_slider_get_value(context.sat) as f32;
    let (r, g, b) = hsv_to_rgb8(hue, sat / 100.0, 1.0);
    lv_obj_set_style_bg_color(context.swatch, color(rgb_hex(r, g, b)), 0);
    set_label(context.hue_label, &format!("{}°", hue.round() as i32));
    set_label(context.sat_label, &format!("{} %", sat.round() as i32));
    if lv_event_get_code(event) != lv_event_code_t_LV_EVENT_RELEASED {
        return;
    }
    let (x, y) = rgb8_to_xy(r, g, b);
    if let Err(error) = post_property(
        context.entity,
        PROPERTY_COLOR,
        ACTION_SET,
        CommandValue::Xy { x, y },
    ) {
        log::warn!("display: color rejected: {error:?}");
    }
}

unsafe extern "C" fn free_color_context(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *mut ColorCtx;
    if !context.is_null() {
        drop(Box::from_raw(context));
    }
}

struct AutoCtx {
    automation: u64,
}

unsafe extern "C" fn on_automation(event: *mut lv_event_t) {
    let target = lv_event_get_target_obj(event);
    let context = lv_event_get_user_data(event) as *const AutoCtx;
    if target.is_null() || context.is_null() {
        return;
    }
    let on = lv_obj_has_state(target, lv_state_t_LV_STATE_CHECKED);
    if let Err(error) = post_automation((*context).automation, on) {
        log::warn!("display: automation toggle rejected: {error:?}");
    }
}

unsafe extern "C" fn free_auto_context(event: *mut lv_event_t) {
    let context = lv_event_get_user_data(event) as *mut AutoCtx;
    if !context.is_null() {
        drop(Box::from_raw(context));
    }
}

unsafe extern "C" fn on_nav(event: *mut lv_event_t) {
    let nav = lv_event_get_user_data(event) as *const Nav;
    if !nav.is_null() {
        NAV.lock().unwrap().push_back(*nav);
    }
}

unsafe extern "C" fn free_nav(event: *mut lv_event_t) {
    let nav = lv_event_get_user_data(event) as *mut Nav;
    if !nav.is_null() {
        drop(Box::from_raw(nav));
    }
}

unsafe fn nav_target(obj: *mut lv_obj_t, nav: Nav) {
    let ptr = Box::into_raw(Box::new(nav));
    lv_obj_add_flag(obj, lv_obj_flag_t_LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        obj,
        Some(on_nav),
        lv_event_code_t_LV_EVENT_CLICKED,
        ptr as *mut _,
    );
    lv_obj_add_event_cb(
        obj,
        Some(free_nav),
        lv_event_code_t_LV_EVENT_DELETE,
        ptr as *mut _,
    );
}

unsafe fn build_widget(
    domain: &DomainHandle,
    row: *mut lv_obj_t,
    entity: EntityId,
    property: PropertyId,
    widgets: &mut Vec<WidgetEntry>,
) {
    let kind = kind_for(property);
    let state = read_state(domain, entity, property);
    match kind {
        WidgetKind::Switch => {
            let control = lv_switch_create(row);
            lv_obj_set_size(control, 80, 40);
            if state.as_ref().and_then(Value::as_bool).unwrap_or(false) {
                lv_obj_add_state(control, lv_state_t_LV_STATE_CHECKED);
            }
            let context = Box::into_raw(Box::new(CmdCtx {
                entity,
                property,
                control,
                locked: AtomicBool::new(false),
                syncing: AtomicBool::new(false),
                has_last: AtomicBool::new(false),
                last_bits: AtomicU32::new(0),
                deadline: Mutex::new(None),
            }));
            lv_obj_add_event_cb(
                control,
                Some(on_control),
                lv_event_code_t_LV_EVENT_VALUE_CHANGED,
                context as *mut _,
            );
            lv_obj_add_event_cb(
                control,
                Some(free_context),
                lv_event_code_t_LV_EVENT_DELETE,
                context as *mut _,
            );
            widgets.push(WidgetEntry {
                entity,
                property,
                kind,
                control,
                value_label: std::ptr::null_mut(),
                ctx: context,
            });
        }
        WidgetKind::Slider => {
            let control = lv_slider_create(row);
            lv_obj_set_size(control, 170, 16);
            let (min, max) = if property == PROPERTY_COLOR_TEMPERATURE {
                (2000, 6500)
            } else {
                (0, 100)
            };
            lv_slider_set_range(control, min, max);
            let initial = state
                .as_ref()
                .and_then(numeric)
                .unwrap_or(min as f32)
                .round() as i32;
            lv_slider_set_value(control, initial.clamp(min, max), false);
            let text = state
                .as_ref()
                .map(|value| format_value(property, value))
                .unwrap_or_else(|| "—".into());
            let value_label = label(row, &text, font_20(), COL_TEXT);
            let context = Box::into_raw(Box::new(CmdCtx {
                entity,
                property,
                control,
                locked: AtomicBool::new(false),
                syncing: AtomicBool::new(false),
                has_last: AtomicBool::new(false),
                last_bits: AtomicU32::new(0),
                deadline: Mutex::new(None),
            }));
            lv_obj_add_event_cb(
                control,
                Some(on_control),
                lv_event_code_t_LV_EVENT_RELEASED,
                context as *mut _,
            );
            lv_obj_add_event_cb(
                control,
                Some(free_context),
                lv_event_code_t_LV_EVENT_DELETE,
                context as *mut _,
            );
            widgets.push(WidgetEntry {
                entity,
                property,
                kind,
                control,
                value_label,
                ctx: context,
            });
        }
        WidgetKind::Value => {
            let text = state
                .as_ref()
                .map(|value| format_value(property, value))
                .unwrap_or_else(|| "—".into());
            let value_label = label(row, &text, font_32(), COL_TEXT);
            widgets.push(WidgetEntry {
                entity,
                property,
                kind,
                control: value_label,
                value_label,
                ctx: std::ptr::null_mut(),
            });
        }
    }
}

unsafe fn build_color_widget(
    domain: &DomainHandle,
    card: *mut lv_obj_t,
    entity: EntityId,
    widgets: &mut Vec<WidgetEntry>,
    colors: &mut Vec<ColorEntry>,
) {
    let column = transparent_box(card, 400, 200);
    lv_obj_set_flex_flow(column, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 8, 0);
    lv_obj_set_flex_align(
        column,
        0,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );

    let swatch = lv_obj_create(column);
    lv_obj_set_size(swatch, 400, 44);
    lv_obj_set_style_radius(swatch, 12, 0);
    lv_obj_set_style_border_width(swatch, 2, 0);
    lv_obj_set_style_bg_opa(swatch, 255, 0);
    lv_obj_set_scrollable(swatch, false);

    let x = read_state(domain, entity, PROPERTY_COLOR_X)
        .as_ref()
        .and_then(numeric)
        .unwrap_or(0.25);
    let y = read_state(domain, entity, PROPERTY_COLOR_Y)
        .as_ref()
        .and_then(numeric)
        .unwrap_or(0.33);
    let (r, g, b) = xy_to_rgb8(x, y);
    lv_obj_set_style_bg_color(swatch, color(rgb_hex(r, g, b)), 0);
    let (hue, sat, _) = rgb8_to_hsv(r, g, b);

    let hue_row = transparent_box(column, 400, 40);
    lv_obj_set_flex_flow(hue_row, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        hue_row,
        lv_flex_align_t_LV_FLEX_ALIGN_SPACE_BETWEEN,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );
    label(hue_row, "Оттенок", font_20(), COL_MUTED);
    let hue_slider = lv_slider_create(hue_row);
    lv_obj_set_size(hue_slider, 190, 16);
    lv_slider_set_range(hue_slider, 0, 359);
    lv_slider_set_value(hue_slider, hue.round() as i32, false);
    let hue_label = label(
        hue_row,
        &format!("{}°", hue.round() as i32),
        font_20(),
        COL_TEXT,
    );

    let sat_row = transparent_box(column, 400, 40);
    lv_obj_set_flex_flow(sat_row, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        sat_row,
        lv_flex_align_t_LV_FLEX_ALIGN_SPACE_BETWEEN,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );
    label(sat_row, "Насыщенность", font_20(), COL_MUTED);
    let sat_slider = lv_slider_create(sat_row);
    lv_obj_set_size(sat_slider, 160, 16);
    lv_slider_set_range(sat_slider, 0, 100);
    lv_slider_set_value(sat_slider, (sat * 100.0).round() as i32, false);
    let sat_label = label(
        sat_row,
        &format!("{} %", (sat * 100.0).round() as i32),
        font_20(),
        COL_TEXT,
    );

    let bright_row = transparent_box(column, 400, 40);
    lv_obj_set_flex_flow(bright_row, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        bright_row,
        lv_flex_align_t_LV_FLEX_ALIGN_SPACE_BETWEEN,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );
    label(bright_row, "Яркость", font_20(), COL_MUTED);
    build_widget(domain, bright_row, entity, PROPERTY_BRIGHTNESS, widgets);

    for slider in [hue_slider, sat_slider] {
        let context = Box::into_raw(Box::new(ColorCtx {
            entity,
            hue: hue_slider,
            sat: sat_slider,
            swatch,
            hue_label,
            sat_label,
        }));
        lv_obj_add_event_cb(
            slider,
            Some(on_color),
            lv_event_code_t_LV_EVENT_VALUE_CHANGED,
            context as *mut _,
        );
        lv_obj_add_event_cb(
            slider,
            Some(on_color),
            lv_event_code_t_LV_EVENT_RELEASED,
            context as *mut _,
        );
        lv_obj_add_event_cb(
            slider,
            Some(free_color_context),
            lv_event_code_t_LV_EVENT_DELETE,
            context as *mut _,
        );
    }

    colors.push(ColorEntry {
        entity,
        swatch,
        hue: hue_slider,
        sat: sat_slider,
        hue_label,
        sat_label,
        x,
        y,
    });
}

unsafe fn build_automation_card(
    domain: &DomainHandle,
    parent: *mut lv_obj_t,
    automation: u64,
    title: &str,
    automations: &mut Vec<AutoEntry>,
) {
    let card = transparent_box(parent, CARD_W, 76);
    lv_obj_set_style_bg_color(card, color(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, 255, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        card,
        lv_flex_align_t_LV_FLEX_ALIGN_SPACE_BETWEEN,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
    );
    let title_label = label(card, title, font_20(), COL_TEXT);

    let control = lv_switch_create(card);
    lv_obj_set_size(control, 80, 40);
    if automation_enabled(domain, automation) {
        lv_obj_add_state(control, lv_state_t_LV_STATE_CHECKED);
    }
    let context = Box::into_raw(Box::new(AutoCtx { automation }));
    lv_obj_add_event_cb(
        control,
        Some(on_automation),
        lv_event_code_t_LV_EVENT_VALUE_CHANGED,
        context as *mut _,
    );
    lv_obj_add_event_cb(
        control,
        Some(free_auto_context),
        lv_event_code_t_LV_EVENT_DELETE,
        context as *mut _,
    );
    automations.push(AutoEntry {
        automation,
        title: title_label,
        switch: control,
    });
}

unsafe fn build_card(
    domain: &DomainHandle,
    parent: *mut lv_obj_t,
    entity: EntityId,
    item_title: &str,
    widgets: &mut Vec<WidgetEntry>,
    colors: &mut Vec<ColorEntry>,
) {
    let properties = capabilities(domain, entity);
    let name = if item_title.is_empty() {
        entity_name(domain, entity)
    } else {
        item_title.to_string()
    };
    let mut rows_height = 0;
    for property in &properties {
        rows_height += if *property == PROPERTY_COLOR { 200 } else { 46 };
    }
    let spacing = properties.len() as i32 * 8;
    let card = transparent_box(parent, CARD_W, 72 + rows_height + spacing);
    lv_obj_set_style_bg_color(card, color(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, 255, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        card,
        0,
        lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        lv_flex_align_t_LV_FLEX_ALIGN_START,
    );

    label(card, &name, font_20(), COL_TEXT);

    for property in properties {
        if property == PROPERTY_COLOR {
            build_color_widget(domain, card, entity, widgets, colors);
            continue;
        }
        let row = transparent_box(card, 400, 46);
        lv_obj_set_flex_flow(row, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(
            row,
            lv_flex_align_t_LV_FLEX_ALIGN_SPACE_BETWEEN,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
        );
        label(row, property_name(property), font_20(), COL_MUTED);
        build_widget(domain, row, entity, property, widgets);
    }
}

unsafe fn build_page(domain: &DomainHandle, groups: &[GroupRef], index: usize) -> Page {
    let screen = lv_obj_create(std::ptr::null_mut());
    lv_obj_set_style_bg_color(screen, color(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, 255, 0);
    lv_obj_set_scrollable(screen, false);

    let mut widgets = Vec::new();
    let mut automations = Vec::new();
    let mut colors = Vec::new();
    let title = if groups.is_empty() {
        "Нет экранов"
    } else {
        groups[index].title.as_str()
    };
    let heading = label(screen, title, font_32(), COL_TEXT);
    lv_obj_align(heading, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 10);

    if !groups.is_empty() {
        let left = label(screen, ICON_LEFT, font_icons(), COL_TEXT);
        lv_obj_align(left, lv_align_t_LV_ALIGN_TOP_LEFT, 12, STATUSBAR_H + 12);
        nav_target(left, Nav::Delta(-1));

        let right = label(screen, ICON_RIGHT, font_icons(), COL_TEXT);
        lv_obj_align(right, lv_align_t_LV_ALIGN_TOP_RIGHT, -12, STATUSBAR_H + 12);
        nav_target(right, Nav::Delta(1));

        let list = lv_obj_create(screen);
        lv_obj_set_size(list, LIST_W, LIST_H);
        lv_obj_align(list, lv_align_t_LV_ALIGN_TOP_MID, 0, LIST_Y);
        lv_obj_set_style_bg_opa(list, 0, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_pad_all(list, 10, 0);
        lv_obj_set_style_pad_row(list, 12, 0);
        lv_obj_set_flex_flow(list, lv_flex_flow_t_LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(
            list,
            0,
            lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            lv_flex_align_t_LV_FLEX_ALIGN_START,
        );

        for item in collect_page_items(domain, groups[index].id) {
            match item {
                PageItem::Entity(item) => build_card(
                    domain,
                    list,
                    item.entity,
                    &item.title,
                    &mut widgets,
                    &mut colors,
                ),
                PageItem::Automation(item) => build_automation_card(
                    domain,
                    list,
                    item.automation,
                    &item.title,
                    &mut automations,
                ),
            }
        }

        if groups.len() > 1 {
            let dots = transparent_box(screen, 0, 20);
            lv_obj_set_width(dots, (groups.len().min(24) as i32) * 22);
            lv_obj_align(dots, lv_align_t_LV_ALIGN_BOTTOM_MID, 0, -6);
            lv_obj_set_style_pad_column(dots, 10, 0);
            lv_obj_set_flex_flow(dots, lv_flex_flow_t_LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(
                dots,
                lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
                lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
                lv_flex_align_t_LV_FLEX_ALIGN_CENTER,
            );
            for i in 0..groups.len().min(24) {
                let dot = lv_obj_create(dots);
                lv_obj_set_size(dot, 12, 12);
                lv_obj_set_style_radius(dot, 6, 0);
                lv_obj_set_style_border_width(dot, 0, 0);
                lv_obj_set_style_bg_color(
                    dot,
                    color(if i == index { COL_ACCENT } else { COL_CHIP }),
                    0,
                );
                lv_obj_set_style_bg_opa(dot, 255, 0);
                lv_obj_set_style_pad_all(dot, 0, 0);
                lv_obj_set_scrollable(dot, false);
                nav_target(dot, Nav::Index(i));
            }
        }
    }

    lv_screen_load(screen);
    Page {
        screen,
        widgets,
        colors,
        automations,
    }
}

unsafe fn replace_page(page: Page) {
    let old = PAGE.lock().unwrap().take();
    lv_screen_load(page.screen);
    if let Some(old) = old {
        lv_obj_delete(old.screen);
    }
    *PAGE.lock().unwrap() = Some(page);
}

/// Rebuild pages from Domain (groups, items, capabilities, names).
fn rebuild(domain: &DomainHandle) {
    if !STARTED.load(Ordering::Relaxed) {
        return;
    }
    let groups = collect_groups(domain);
    *GROUPS.lock().unwrap() = groups.clone();
    if VIEW.load(Ordering::Relaxed) != View::Page as u8 {
        return;
    }
    let current = CURRENT.load(Ordering::Relaxed);
    let index = if groups.is_empty() {
        0
    } else {
        current.min(groups.len() - 1)
    };
    if !lock() {
        log::warn!("display: lock timeout in rebuild");
        return;
    }
    unsafe {
        let page = build_page(domain, &groups, index);
        log::debug!(
            "display: page rebuilt groups={} index={} widgets={} colors={}",
            groups.len(),
            index,
            page.widgets.len(),
            page.colors.len()
        );
        replace_page(page);
    }
    CURRENT.store(index, Ordering::Relaxed);
    unlock();
}

/// Pending navigation from touch (arrows/menu; the main loop drains the queue).
pub fn take_nav() -> Option<Nav> {
    NAV.lock().unwrap().pop_front()
}

pub fn navigate(domain: &DomainHandle, nav: Nav) {
    if !STARTED.load(Ordering::Relaxed) {
        return;
    }
    match nav {
        Nav::Delta(_) | Nav::Index(_) => {
            let groups = GROUPS.lock().unwrap().clone();
            if groups.is_empty() {
                return;
            }
            let current = CURRENT.load(Ordering::Relaxed);
            let len = groups.len();
            let next = match nav {
                Nav::Delta(delta) => {
                    let mut next = current as i32 + delta;
                    next = ((next % len as i32) + len as i32) % len as i32;
                    next as usize
                }
                Nav::Index(index) => index.min(len - 1),
                _ => return,
            };
            if next == current {
                return;
            }
            if !lock() {
                return;
            }
            unsafe {
                let page = build_page(domain, &groups, next);
                replace_page(page);
            }
            CURRENT.store(next, Ordering::Relaxed);
            unlock();
            VIEW.store(View::Page as u8, Ordering::Relaxed);
        }
        Nav::Back => {
            let groups = GROUPS.lock().unwrap().clone();
            let current = CURRENT.load(Ordering::Relaxed);
            let index = if groups.is_empty() {
                0
            } else {
                current.min(groups.len() - 1)
            };
            if !lock() {
                return;
            }
            unsafe {
                hide_menu();
                close_wifi_dialog();
                let page = build_page(domain, &groups, index);
                replace_page(page);
            }
            CURRENT.store(index, Ordering::Relaxed);
            unlock();
            VIEW.store(View::Page as u8, Ordering::Relaxed);
        }
        Nav::OpenSettings => {
            if !lock() {
                return;
            }
            unsafe {
                hide_menu();
                close_wifi_dialog();
                let page = build_settings(domain);
                replace_page(page);
            }
            unlock();
            VIEW.store(View::Settings as u8, Ordering::Relaxed);
        }
        Nav::OpenWifi => {
            if !lock() {
                return;
            }
            unsafe {
                hide_menu();
                let page = build_wifi(domain);
                replace_page(page);
            }
            unlock();
            VIEW.store(View::Wifi as u8, Ordering::Relaxed);
        }
        Nav::WifiScan => { /* handled by the main loop (blocking C scan), not a screen switch */ }
    }
}

pub fn drain(domain: &DomainHandle, receiver: &Receiver<Fact>) {
    let mut rebuild_needed = false;
    while let Ok(fact) = receiver.try_recv() {
        match fact.kind {
            FactKind::EntityUpserted if fact.entity == ENTITY_PROPERTY_STATE => {
                let key = fact.key();
                if key.len() < 16 {
                    continue;
                }
                let Some(key) = StateKey::from_bytes(key) else {
                    continue;
                };
                let entity = key.entity;
                let property = key.property;
                if entity == ENTITY_SYSTEM
                    && matches!(property, PROPERTY_SYSTEM_HOUR | PROPERTY_SYSTEM_MINUTE)
                {
                    let Some(value) = numeric(&fact.value) else {
                        continue;
                    };
                    let mut clock = CLOCK_LAST.lock().unwrap();
                    if property == PROPERTY_SYSTEM_HOUR {
                        clock.0 = value.round().clamp(0.0, 23.0) as u8;
                    } else {
                        clock.1 = value.round().clamp(0.0, 59.0) as u8;
                    }
                    let (hour, minute) = *clock;
                    drop(clock);
                    set_clock(hour, minute);
                    continue;
                }
                on_state(entity, property, fact.value);
            }
            FactKind::EntityUpserted | FactKind::EntityRemoved => match fact.entity {
                ENTITY_LOCATION => {
                    if fact.kind == FactKind::EntityUpserted {
                        let mut bytes = [0u8; LOCATION_RECORD_LEN];
                        if domain
                            .entity_get(ENTITY_LOCATION, fact.key(), &mut bytes)
                            .is_ok()
                        {
                            if let Some(location) = LocationRecord::decode(&bytes) {
                                set_location(&location_text(&location));
                            }
                        }
                    }
                }
                ENTITY_WEATHER => {
                    if fact.kind == FactKind::EntityUpserted {
                        let mut bytes = [0u8; WEATHER_RECORD_LEN];
                        if domain
                            .entity_get(ENTITY_WEATHER, fact.key(), &mut bytes)
                            .is_ok()
                        {
                            if let Some(weather) = WeatherRecord::decode(&bytes) {
                                set_weather(
                                    weather.condition,
                                    f32::from(weather.temperature_c100) / 100.0,
                                    f32::from(weather.humidity_p100) / 100.0,
                                );
                            }
                        }
                    }
                }
                ENTITY_SETTINGS => {
                    if fact.kind == FactKind::EntityUpserted {
                        let mut bytes = [0u8; SETTINGS_RECORD_LEN];
                        if domain
                            .entity_get(ENTITY_SETTINGS, fact.key(), &mut bytes)
                            .is_ok()
                        {
                            if let Some(settings) = SettingsRecord::decode(&bytes) {
                                set_brightness(settings.brightness_pct);
                            }
                        }
                    }
                }
                ENTITY_AUTOMATION => {
                    if fact.kind == FactKind::EntityUpserted {
                        if let Ok(id) = fact.key().try_into().map(u64::from_le_bytes) {
                            refresh_automation(domain, id);
                        }
                    } else {
                        rebuild_needed = true;
                    }
                }
                ENTITY_LOGICAL_DEVICE
                | ENTITY_ENTITY
                | ENTITY_CAPABILITIES
                | ENTITY_GROUP
                | ENTITY_GROUP_ITEM
                | ENTITY_GROUP_AUTOMATION => rebuild_needed = true,
                ENTITY_WIFI_STATUS => {
                    if fact.kind == FactKind::EntityUpserted {
                        if let Some(status) = wifi_status(domain) {
                            set_wifi((status.connected != 0).then_some(i16::from(status.rssi)));
                        }
                    }
                    refresh_wifi(domain);
                }
                ENTITY_WIFI_SCAN => refresh_wifi(domain),
                _ => {}
            },
            _ => {}
        }
    }
    if rebuild_needed {
        rebuild(domain);
    }
}

fn refresh_automation(domain: &DomainHandle, automation: u64) {
    let mut bytes = [0u8; AUTOMATION_RECORD_LEN];
    if domain
        .entity_get(ENTITY_AUTOMATION, &automation.to_le_bytes(), &mut bytes)
        .is_err()
    {
        return;
    }
    let Some(rule) = RuleRecord::decode(&bytes).ok() else {
        return;
    };
    let entries: Vec<(*mut lv_obj_t, *mut lv_obj_t)> = {
        let page = PAGE.lock().unwrap();
        let Some(page) = page.as_ref() else {
            return;
        };
        page.automations
            .iter()
            .filter(|entry| entry.automation == automation)
            .map(|entry| (entry.title, entry.switch))
            .collect()
    };
    if entries.is_empty() || !lock() {
        return;
    }
    unsafe {
        for (title, switch) in entries {
            if rule.name[0] != 0 {
                set_label(title, &c_string(&rule.name));
            }
            if rule.enabled != 0 {
                lv_obj_add_state(switch, lv_state_t_LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(switch, lv_state_t_LV_STATE_CHECKED);
            }
        }
    }
    unlock();
}
fn location_text(location: &LocationRecord) -> String {
    let end = location
        .name
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(location.name.len());
    String::from_utf8_lossy(&location.name[..end]).into_owned()
}

/// Apply one confirmed property state to the widgets of the current page.
fn on_state(entity: EntityId, property: PropertyId, value: Value) {
    if property == PROPERTY_COLOR_X || property == PROPERTY_COLOR_Y {
        let Some(number) = numeric(&value) else {
            return;
        };
        let entries: Vec<ColorEntry> = {
            let mut guard = PAGE.lock().unwrap();
            let Some(page) = guard.as_mut() else {
                return;
            };
            for entry in &mut page.colors {
                if entry.entity != entity {
                    continue;
                }
                if property == PROPERTY_COLOR_X {
                    entry.x = number;
                } else {
                    entry.y = number;
                }
            }
            page.colors
                .iter()
                .filter(|entry| entry.entity == entity)
                .copied()
                .collect()
        };
        if entries.is_empty() {
            return;
        }
        if !lock() {
            return;
        }
        for entry in entries {
            let (r, g, b) = xy_to_rgb8(entry.x, entry.y);
            let (hue, sat, _) = rgb8_to_hsv(r, g, b);
            unsafe {
                lv_obj_set_style_bg_color(entry.swatch, color(rgb_hex(r, g, b)), 0);
                lv_slider_set_value(entry.hue, hue.round() as i32, false);
                lv_slider_set_value(entry.sat, (sat * 100.0).round() as i32, false);
            }
            set_label(entry.hue_label, &format!("{}°", hue.round() as i32));
            set_label(
                entry.sat_label,
                &format!("{} %", (sat * 100.0).round() as i32),
            );
        }
        unlock();
        return;
    }

    let entries: Vec<(WidgetKind, *mut lv_obj_t, *mut lv_obj_t, *mut CmdCtx)> = {
        let page = PAGE.lock().unwrap();
        let Some(page) = page.as_ref() else {
            return;
        };
        page.widgets
            .iter()
            .filter(|widget| widget.entity == entity && widget.property == property)
            .map(|widget| (widget.kind, widget.control, widget.value_label, widget.ctx))
            .collect()
    };
    if entries.is_empty() {
        return;
    }
    if !lock() {
        return;
    }
    for (kind, control, value_label, ctx) in entries {
        match kind {
            WidgetKind::Switch => {
                let Some(on) = value.as_bool() else {
                    continue;
                };
                if ctx.is_null() {
                    continue;
                }
                let ctx = unsafe { &*ctx };
                if ctx.syncing.load(Ordering::Relaxed) {
                    continue;
                }
                let bits = u32::from(on);
                let has_last = ctx.has_last.load(Ordering::Relaxed);
                let last_bits = ctx.last_bits.load(Ordering::Relaxed);
                if ctx.locked.load(Ordering::Relaxed) {
                    if has_last && bits == last_bits {
                        continue;
                    }
                    log::debug!(
                        "display: switch fresh entity={:016x} bits={} last={} unlock",
                        ctx.entity,
                        bits,
                        last_bits
                    );
                    *ctx.deadline.lock().unwrap() = None;
                    ctx.locked.store(false, Ordering::Relaxed);
                    unsafe { lv_obj_remove_state(control, lv_state_t_LV_STATE_DISABLED) };
                } else if has_last && bits == last_bits {
                    continue;
                }
                log::debug!("display: switch apply entity={:016x} on={}", ctx.entity, on);
                ctx.syncing.store(true, Ordering::Relaxed);
                unsafe {
                    if on {
                        lv_obj_add_state(control, lv_state_t_LV_STATE_CHECKED);
                    } else {
                        lv_obj_remove_state(control, lv_state_t_LV_STATE_CHECKED);
                    }
                }
                ctx.syncing.store(false, Ordering::Relaxed);
                ctx.last_bits.store(bits, Ordering::Relaxed);
                ctx.has_last.store(true, Ordering::Relaxed);
            }
            WidgetKind::Slider => {
                if unsafe { lv_obj_has_state(control, lv_state_t_LV_STATE_PRESSED) } {
                    continue;
                }
                if let Some(number) = numeric(&value) {
                    let (min, max) = if property == PROPERTY_COLOR_TEMPERATURE {
                        (2000, 6500)
                    } else {
                        (0, 100)
                    };
                    unsafe {
                        lv_slider_set_value(control, (number.round() as i32).clamp(min, max), false)
                    };
                    set_label(value_label, &format_value(property, &value));
                }
            }
            WidgetKind::Value => {
                set_label(value_label, &format_value(property, &value));
            }
        }
    }
    unlock();
}

/// V2-style switch lock: re-enable a switch after the command settles, even if
/// the domain never reports a fresh value.
pub fn tick() {
    let contexts: Vec<*mut CmdCtx> = {
        let page = PAGE.lock().unwrap();
        page.as_ref()
            .map(|page| {
                page.widgets
                    .iter()
                    .filter(|widget| widget.kind == WidgetKind::Switch && !widget.ctx.is_null())
                    .map(|widget| widget.ctx)
                    .collect()
            })
            .unwrap_or_default()
    };
    if contexts.is_empty() {
        return;
    }
    let now = Instant::now();
    for ctx in contexts {
        let ctx = unsafe { &*ctx };
        if !ctx.locked.load(Ordering::Relaxed) {
            continue;
        }
        let expired = ctx
            .deadline
            .lock()
            .unwrap()
            .map(|deadline| now >= deadline)
            .unwrap_or(false);
        if !expired {
            continue;
        }
        if !lock() {
            return;
        }
        log::debug!("display: switch unlock timeout entity={:016x}", ctx.entity);
        ctx.locked.store(false, Ordering::Relaxed);
        *ctx.deadline.lock().unwrap() = None;
        if !ctx.control.is_null() {
            unsafe { lv_obj_remove_state(ctx.control, lv_state_t_LV_STATE_DISABLED) };
        }
        unlock();
    }
}
