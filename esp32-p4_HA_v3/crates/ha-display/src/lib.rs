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
use std::sync::mpsc::{sync_channel, Receiver};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use ha_domain::canonical::{
    CanonicalCommand, CapabilitiesRecord, CommandValue, EntityId, EntityRecord, GroupAutomationKey,
    GroupItemKey, GroupItemRecord, GroupKey, GroupRecord, LocationRecord, PropertyId,
    RuleRecord, SettingsRecord, StateKey, WeatherRecord, WifiScanRecord,
    WifiStatusRecord, ACTION_OFF, ACTION_ON, ACTION_SET, AUTOMATION_RECORD_LEN,
    CAPABILITIES_LEN, COMMAND_AUTOMATION_ENABLE, COMMAND_PROPERTY, COMMAND_SETTINGS,
    COMMAND_WIFI_CONNECT, COMMAND_WIFI_SCAN, ENTITY_AUTOMATION, ENTITY_CAPABILITIES,
    ENTITY_ENTITY,
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
use ha_domain::{DomainHandle, Fact, FactKind, FactMeta, Filter, Value};

mod color;
mod model;
mod settings;
mod wifi;
mod pages;
mod widgets;
mod commands;
mod menu;
mod status;
mod state;
use color::*;
use model::*;
use settings::*;
use wifi::*;
use pages::{build_page, replace_page};
use widgets::*;
use commands::{post_automation, post_property, post_settings, post_wifi_connect};
use menu::{build_menu, hide_menu, on_bar_pressed, on_bar_released};
use status::{build_status_bar, set_clock, set_location, set_weather, set_wifi, sync_clock};
pub use state::{drain, navigate, start, take_nav, tick};
pub use commands::request_wifi_scan;

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
static DISPLAY_RX: Mutex<Option<Receiver<Fact>>> = Mutex::new(None);
static LAST_CLOCK_POLL: Mutex<Option<Instant>> = Mutex::new(None);
fn domain() -> Result<DomainHandle, ha_domain::Error> {
    DOMAIN
        .lock()
        .unwrap()
        .clone()
        .ok_or(ha_domain::Error::InvalidState)
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

struct ColorCtx {
    entity: EntityId,
    hue: *mut lv_obj_t,
    sat: *mut lv_obj_t,
    swatch: *mut lv_obj_t,
    hue_label: *mut lv_obj_t,
    sat_label: *mut lv_obj_t,
}

struct AutoCtx {
    automation: u64,
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
