//! Display service. Panel/touch/LVGL port live in C (`native_display`); screens,
//! state and all widget calls are driven from Rust through the LVGL bindings.
//!
//! LVGL is not thread safe: the C port task renders, so every access from here is
//! made under `ha_display_lock`.

#![allow(warnings, clippy::all)]

include!(concat!(env!("OUT_DIR"), "/lvgl_bindings.rs"));

use std::collections::VecDeque;
use std::ffi::{c_char, CStr, CString};
use std::ptr::{addr_of, null_mut};
use std::sync::atomic::{
    AtomicBool, AtomicI32, AtomicPtr, AtomicU32, AtomicU8, AtomicUsize, Ordering,
};
use std::sync::mpsc::Receiver;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use ha_domain::canonical::{
    CommandValue, EntityId, LocationRecord, PropertyId, SettingsRecord, StateKey, WeatherRecord,
    ACTION_OFF, ACTION_ON, ACTION_SET, AUTOMATION_RECORD_LEN, CAPABILITIES_LEN, ENTITY_AUTOMATION,
    ENTITY_CAPABILITIES, ENTITY_ENTITY, ENTITY_GROUP, ENTITY_GROUP_AUTOMATION, ENTITY_GROUP_ITEM,
    ENTITY_LOCATION, ENTITY_LOGICAL_DEVICE, ENTITY_PROPERTY_STATE, ENTITY_SETTINGS, ENTITY_SYSTEM,
    ENTITY_WEATHER, LOCATION_RECORD_LEN, PROPERTY_BATTERY_PERCENT, PROPERTY_BATTERY_VOLTAGE,
    PROPERTY_BRIGHTNESS, PROPERTY_COLOR, PROPERTY_COLOR_TEMPERATURE, PROPERTY_COLOR_X,
    PROPERTY_COLOR_Y, PROPERTY_HUMIDITY, PROPERTY_ILLUMINANCE, PROPERTY_OCCUPANCY, PROPERTY_POWER,
    PROPERTY_SYSTEM_HOUR, PROPERTY_SYSTEM_MINUTE, PROPERTY_TEMPERATURE, SETTINGS_ID,
    SETTINGS_RECORD_LEN, STATE_RECORD_LEN, WEATHER_CLEAR, WEATHER_CLEAR_NIGHT, WEATHER_CLOUDY,
    WEATHER_FOG, WEATHER_HAIL, WEATHER_LIGHTNING, WEATHER_LIGHTNING_RAINY, WEATHER_OVERCAST,
    WEATHER_PARTLYCLOUDY, WEATHER_POURING, WEATHER_RAINY, WEATHER_RECORD_LEN, WEATHER_SNOWY,
    WEATHER_SNOWY_RAINY, WEATHER_WINDY, WEATHER_WINDY_VARIANT,
};
use ha_domain::{DomainHandle, Fact, FactKind, Value};

use crate::intake;

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

/// Bring up the panel, touch, the LVGL port and the persistent status bar.
pub fn start() -> bool {
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
    STARTED.store(true, Ordering::Relaxed);
    log::info!("display: port ready");
    true
}

/// Local clock (status bar), 24-hour.
pub fn set_clock(hour: u8, minute: u8) {
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
pub fn set_location(name: &str) {
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
pub fn set_weather(condition: u8, temperature_c: f32, humidity_pct: f32) {
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
pub fn set_wifi(rssi: Option<i16>) {
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

#[derive(Clone)]
struct GroupRef {
    id: u64,
    title: String,
}

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
static WIFI_SCAN: Mutex<Vec<(String, i8)>> = Mutex::new(Vec::new());
static WIFI_SCAN_RUN: AtomicBool = AtomicBool::new(false);
static WIFI_SCANNING: AtomicBool = AtomicBool::new(false);
static WIFI_DIALOG: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static WIFI_KEYBOARD: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static WIFI_BACKDROP: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static WIFI_TEXTAREA: AtomicPtr<lv_obj_t> = AtomicPtr::new(null_mut());
static DIALOG_SSID: Mutex<Option<String>> = Mutex::new(None);
static CLOCK_LAST: Mutex<(u8, u8)> = Mutex::new((0, 0));

extern "C" {
    fn ha_wifi_status(ssid: *mut c_char, len: usize, rssi: *mut i32) -> i32;
}

fn c_string(bytes: &[u8]) -> String {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

fn read_state(domain: &DomainHandle, entity: EntityId, property: PropertyId) -> Option<Value> {
    let key = StateKey { entity, property }.to_bytes();
    let mut bytes = [0u8; STATE_RECORD_LEN];
    domain
        .entity_get(ENTITY_PROPERTY_STATE, &key, &mut bytes)
        .ok()?;
    Value::from_bytes(&bytes)
}

fn collect_groups(domain: &DomainHandle) -> Vec<GroupRef> {
    let Ok(snapshot) = domain.entity_snapshot(ENTITY_GROUP) else {
        return Vec::new();
    };
    let mut groups = Vec::new();
    for index in 0..snapshot.len() {
        let (Some(key), Some(record)) = (snapshot.key(index), snapshot.record(index)) else {
            continue;
        };
        if key.len() == 8 && record.len() >= 32 {
            groups.push(GroupRef {
                id: u64::from_le_bytes(key.try_into().unwrap()),
                title: c_string(&record[..32]),
            });
        }
    }
    groups.sort_by(|a, b| a.title.cmp(&b.title));
    groups
}

struct Item {
    order: u16,
    entity: EntityId,
    title: String,
}

fn collect_items(domain: &DomainHandle, group: u64) -> Vec<Item> {
    let Ok(snapshot) = domain.entity_snapshot(ENTITY_GROUP_ITEM) else {
        return Vec::new();
    };
    let mut items = Vec::new();
    for index in 0..snapshot.len() {
        let (Some(key), Some(record)) = (snapshot.key(index), snapshot.record(index)) else {
            continue;
        };
        if key.len() != 16 || record.len() < 36 {
            continue;
        }
        let group_id = u64::from_le_bytes(key[..8].try_into().unwrap());
        if group_id != group {
            continue;
        }
        items.push(Item {
            entity: u64::from_le_bytes(key[8..16].try_into().unwrap()),
            order: u16::from_le_bytes(record[..2].try_into().unwrap()),
            title: c_string(&record[4..36]),
        });
    }
    items.sort_by_key(|item| item.order);
    items
}

struct AutomationItem {
    order: u16,
    automation: u64,
    title: String,
}

fn collect_group_automations(domain: &DomainHandle, group: u64) -> Vec<AutomationItem> {
    let Ok(snapshot) = domain.entity_snapshot(ENTITY_GROUP_AUTOMATION) else {
        return Vec::new();
    };
    let mut items = Vec::new();
    for index in 0..snapshot.len() {
        let (Some(key), Some(record)) = (snapshot.key(index), snapshot.record(index)) else {
            continue;
        };
        if key.len() != 16 || record.len() < 36 {
            continue;
        }
        let group_id = u64::from_le_bytes(key[..8].try_into().unwrap());
        if group_id != group {
            continue;
        }
        items.push(AutomationItem {
            automation: u64::from_le_bytes(key[8..16].try_into().unwrap()),
            order: u16::from_le_bytes(record[..2].try_into().unwrap()),
            title: c_string(&record[4..36]),
        });
    }
    items.sort_by_key(|item| item.order);
    items
}

enum PageItem {
    Entity(Item),
    Automation(AutomationItem),
}

fn collect_page_items(domain: &DomainHandle, group: u64) -> Vec<PageItem> {
    let mut entities = collect_items(domain, group);
    entities.reverse();
    let mut automations = collect_group_automations(domain, group);
    automations.reverse();
    let mut out = Vec::new();
    while !entities.is_empty() || !automations.is_empty() {
        let take_entity = match (entities.last(), automations.last()) {
            (Some(entity), Some(automation)) => entity.order <= automation.order,
            (Some(_), None) => true,
            _ => false,
        };
        if take_entity {
            out.push(PageItem::Entity(entities.pop().unwrap()));
        } else {
            out.push(PageItem::Automation(automations.pop().unwrap()));
        }
    }
    out
}

fn automation_enabled(domain: &DomainHandle, automation: u64) -> bool {
    let mut bytes = [0u8; AUTOMATION_RECORD_LEN];
    if domain
        .entity_get(ENTITY_AUTOMATION, &automation.to_le_bytes(), &mut bytes)
        .is_err()
    {
        return false;
    }
    bytes[384] != 0
}

fn entity_name(domain: &DomainHandle, entity: EntityId) -> String {
    let mut bytes = [0u8; ha_domain::canonical::ENTITY_RECORD_LEN];
    if domain
        .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut bytes)
        .is_err()
    {
        return String::new();
    }
    c_string(&bytes[8..40])
}

fn capabilities(domain: &DomainHandle, entity: EntityId) -> Vec<PropertyId> {
    let mut bytes = [0u8; CAPABILITIES_LEN];
    if domain
        .entity_get(ENTITY_CAPABILITIES, &entity.to_le_bytes(), &mut bytes)
        .is_err()
    {
        return Vec::new();
    }
    let count = usize::from(bytes[0]).min(8);
    let mut out = Vec::with_capacity(count);
    for index in 0..count {
        let at = 4 + index * 24;
        let property = u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap());
        if property != 0 {
            out.push(property);
        }
    }
    out
}

fn numeric(value: &Value) -> Option<f32> {
    value
        .as_f32()
        .or_else(|| value.as_i32().map(|v| v as f32))
        .or_else(|| value.as_u32().map(|v| v as f32))
}

fn property_name(property: PropertyId) -> &'static str {
    match property {
        PROPERTY_POWER => "Питание",
        PROPERTY_BRIGHTNESS => "Яркость",
        PROPERTY_COLOR => "Цвет",
        PROPERTY_COLOR_TEMPERATURE => "Температура цвета",
        PROPERTY_TEMPERATURE => "Температура",
        PROPERTY_HUMIDITY => "Влажность",
        PROPERTY_ILLUMINANCE => "Освещённость",
        PROPERTY_OCCUPANCY => "Занятость",
        PROPERTY_BATTERY_PERCENT => "Батарея",
        PROPERTY_BATTERY_VOLTAGE => "Напряжение",
        _ => "Свойство",
    }
}

fn format_value(property: PropertyId, value: &Value) -> String {
    if property == PROPERTY_OCCUPANCY {
        return if value.as_bool().unwrap_or(false) {
            "Занято".into()
        } else {
            "Свободно".into()
        };
    }
    if let Some(on) = value.as_bool() {
        return if on {
            "Вкл".into()
        } else {
            "Выкл".into()
        };
    }
    let Some(number) = numeric(value) else {
        return "—".into();
    };
    match property {
        PROPERTY_TEMPERATURE => format!("{number:.1} °C"),
        PROPERTY_HUMIDITY => format!("{number:.1} %"),
        PROPERTY_ILLUMINANCE => format!("{number:.0} lx"),
        PROPERTY_BATTERY_PERCENT => format!("{number:.0} %"),
        PROPERTY_BATTERY_VOLTAGE => format!("{number:.2} V"),
        PROPERTY_BRIGHTNESS => format!("{number:.0} %"),
        PROPERTY_COLOR_TEMPERATURE => format!("{number:.0} K"),
        _ => format!("{number:.1}"),
    }
}

fn kind_for(property: PropertyId) -> WidgetKind {
    match property {
        PROPERTY_POWER => WidgetKind::Switch,
        PROPERTY_BRIGHTNESS | PROPERTY_COLOR_TEMPERATURE => WidgetKind::Slider,
        _ => WidgetKind::Value,
    }
}

/* --- sRGB <-> linear <-> HSV <-> CIE xy (порт ui_color.c) --------------- */

fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

fn linear_to_srgb(c: f32) -> f32 {
    if c <= 0.0031308 {
        12.92 * c
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    }
}

fn clamp01(value: f32) -> f32 {
    value.clamp(0.0, 1.0)
}

fn rgb8_to_hsv(r8: u8, g8: u8, b8: u8) -> (f32, f32, f32) {
    let r = f32::from(r8) / 255.0;
    let g = f32::from(g8) / 255.0;
    let b = f32::from(b8) / 255.0;
    let max = r.max(g).max(b);
    let min = r.min(g).min(b);
    let delta = max - min;
    let value = max;
    let sat = if max > 0.0 { delta / max } else { 0.0 };
    let hue = if delta <= 0.0 {
        0.0
    } else if max == r {
        60.0 * ((g - b) / delta).rem_euclid(6.0)
    } else if max == g {
        60.0 * ((b - r) / delta + 2.0)
    } else {
        60.0 * ((r - g) / delta + 4.0)
    };
    (hue, sat, value)
}

fn hsv_to_rgb8(hue: f32, sat: f32, value: f32) -> (u8, u8, u8) {
    let c = value * sat;
    let hp = hue.rem_euclid(360.0) / 60.0;
    let x = c * (1.0 - (hp.rem_euclid(2.0) - 1.0).abs());
    let (r, g, b) = match hp as i32 {
        0 => (c, x, 0.0),
        1 => (x, c, 0.0),
        2 => (0.0, c, x),
        3 => (0.0, x, c),
        4 => (x, 0.0, c),
        _ => (c, 0.0, x),
    };
    let m = value - c;
    let to_u8 = |v: f32| ((v + m) * 255.0 + 0.5) as u8;
    (to_u8(r), to_u8(g), to_u8(b))
}

fn rgb8_to_xy(r8: u8, g8: u8, b8: u8) -> (f32, f32) {
    let r = srgb_to_linear(f32::from(r8) / 255.0);
    let g = srgb_to_linear(f32::from(g8) / 255.0);
    let b = srgb_to_linear(f32::from(b8) / 255.0);
    let x = r * 0.4124 + g * 0.3576 + b * 0.1805;
    let y = r * 0.2126 + g * 0.7152 + b * 0.0722;
    let z = r * 0.0193 + g * 0.1192 + b * 0.9505;
    let sum = x + y + z;
    if sum <= 0.0 {
        (0.0, 0.0)
    } else {
        (clamp01(x / sum), clamp01(y / sum))
    }
}

fn xy_to_rgb8(x: f32, y: f32) -> (u8, u8, u8) {
    if y <= 0.0001 {
        return (255, 255, 255);
    }
    let big_x = x / y;
    let big_z = (1.0 - x - y) / y;
    let mut r = big_x * 3.2406 - 1.5372 - big_z * 0.4986;
    let mut g = -big_x * 0.9689 + 1.8758 + big_z * 0.0415;
    let mut b = big_x * 0.0557 - 0.204 + big_z * 1.057;
    let m = r.max(g).max(b).max(1.0);
    r /= m;
    g /= m;
    b /= m;
    let to_u8 = |v: f32| (clamp01(linear_to_srgb(v)) * 255.0 + 0.5) as u8;
    (to_u8(r), to_u8(g), to_u8(b))
}

fn rgb_hex(r: u8, g: u8, b: u8) -> u32 {
    (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b)
}

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
        intake::command(
            context.entity,
            PROPERTY_POWER,
            if on { ACTION_ON } else { ACTION_OFF },
            CommandValue::None,
        )
    } else {
        let value = lv_slider_get_value(target) as f32;
        intake::command(
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
    if let Err(error) = intake::command(
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
    if let Err(error) = intake::set_automation_enabled((*context).automation, on) {
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
    label(card, title, font_20(), COL_TEXT);

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
                PageItem::Automation(item) => {
                    build_automation_card(domain, list, item.automation, &item.title)
                }
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
    }
}

fn display_settings(domain: &DomainHandle) -> SettingsRecord {
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
        if let Err(error) = intake::set_settings(value) {
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

unsafe fn build_settings(domain: &DomainHandle) -> Page {
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
    }
}

pub fn set_brightness(percent: u8) {
    unsafe { ha_display_set_brightness(percent) };
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
pub fn rebuild(domain: &DomainHandle) {
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

unsafe fn close_wifi_dialog() {
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
        if let Err(error) = intake::wifi_connect(ssid, password) {
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

unsafe fn open_wifi_dialog(ssid: &str) {
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

unsafe fn build_wifi(_domain: &DomainHandle) -> Page {
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

    let mut ssid = [0 as c_char; 33];
    let mut rssi = 0i32;
    let connected = ha_wifi_status(ssid.as_mut_ptr(), ssid.len(), &mut rssi) != 0;
    let status_text = if connected {
        format!(
            "{}  {} dBm",
            CStr::from_ptr(ssid.as_ptr()).to_string_lossy(),
            rssi
        )
    } else {
        "не подключено".to_string()
    };
    let status = label(
        screen,
        &status_text,
        font_20(),
        if connected { COL_OK } else { COL_MUTED },
    );
    lv_obj_align(status, lv_align_t_LV_ALIGN_TOP_MID, 0, STATUSBAR_H + 60);

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

    let networks = WIFI_SCAN.lock().unwrap().clone();
    if networks.is_empty() {
        let hint = if WIFI_SCANNING.load(Ordering::Relaxed) {
            "Сканирование…"
        } else if WIFI_SCAN_RUN.load(Ordering::Relaxed) {
            "Сети не найдены"
        } else {
            "Нажмите «Сканировать»"
        };
        label(list, hint, font_20(), COL_MUTED);
    } else {
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

    lv_screen_load(screen);
    Page {
        screen,
        widgets: Vec::new(),
        colors: Vec::new(),
    }
}

pub fn wifi_scan_done(domain: &DomainHandle, results: Vec<(String, i8)>) {
    *WIFI_SCAN.lock().unwrap() = results;
    WIFI_SCAN_RUN.store(true, Ordering::Relaxed);
    WIFI_SCANNING.store(false, Ordering::Relaxed);
    if VIEW.load(Ordering::Relaxed) != View::Wifi as u8 {
        return;
    }
    if !lock() {
        return;
    }
    unsafe {
        let page = build_wifi(domain);
        replace_page(page);
    }
    unlock();
}

/// Seed the Wi-Fi screen with the last cached scan (no rebuild).
pub fn set_wifi_scan(results: Vec<(String, i8)>) {
    *WIFI_SCAN.lock().unwrap() = results;
    WIFI_SCAN_RUN.store(true, Ordering::Relaxed);
}

/// Mark a scan as in flight and refresh the Wi-Fi screen hint.
pub fn wifi_scan_requested(domain: &DomainHandle) {
    WIFI_SCANNING.store(true, Ordering::Relaxed);
    if VIEW.load(Ordering::Relaxed) != View::Wifi as u8 {
        return;
    }
    if !lock() {
        return;
    }
    unsafe {
        let page = build_wifi(domain);
        replace_page(page);
    }
    unlock();
}

/// Drain the Display inbox. Facts are already compact; the widget update is
/// addressed by `(entity, property)` and never rebuilds the whole page.
pub fn drain(domain: &DomainHandle, receiver: &Receiver<Fact>) {
    let mut rebuild_needed = false;
    while let Ok(fact) = receiver.try_recv() {
        match fact.kind {
            FactKind::EntityUpserted if fact.entity == ENTITY_PROPERTY_STATE => {
                let key = fact.key();
                if key.len() < 16 {
                    continue;
                }
                let entity = u64::from_le_bytes(key[..8].try_into().expect("state entity"));
                let property = u32::from_le_bytes(key[8..12].try_into().expect("state property"));
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
                ENTITY_LOGICAL_DEVICE
                | ENTITY_ENTITY
                | ENTITY_CAPABILITIES
                | ENTITY_GROUP
                | ENTITY_GROUP_ITEM
                | ENTITY_GROUP_AUTOMATION
                | ENTITY_AUTOMATION => rebuild_needed = true,
                _ => {}
            },
            _ => {}
        }
    }
    if rebuild_needed {
        rebuild(domain);
    }
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
pub fn on_state(entity: EntityId, property: PropertyId, value: Value) {
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
