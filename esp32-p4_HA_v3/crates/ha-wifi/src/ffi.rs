//! Raw ESP-Hosted Wi-Fi boundary. All pointers/lifetimes are contained here.
use std::ffi::{c_char, CStr, CString};

extern "C" {
    fn ha_wifi_start() -> i32;
    fn ha_wifi_scan_start() -> i32;
    fn ha_wifi_scan_ready() -> i32;
    fn ha_wifi_scan_clear();
    fn ha_wifi_scan_count() -> i32;
    fn ha_wifi_scan_get(
        index: i32,
        ssid: *mut c_char,
        len: usize,
        rssi: *mut i32,
        auth: *mut i32,
    ) -> i32;
    fn ha_wifi_state() -> i32;
    fn ha_wifi_status(ssid: *mut c_char, len: usize, rssi: *mut i32) -> i32;
    fn ha_wifi_connect_ssid(ssid: *const c_char, password: *const c_char) -> i32;
    fn ha_wifi_known_add(ssid: *const c_char, password: *const c_char) -> i32;
    fn ha_wifi_known_remove(ssid: *const c_char) -> i32;
    fn ha_wifi_known_count() -> i32;
    fn ha_wifi_known_get(
        index: i32,
        ssid: *mut c_char,
        len: usize,
        password: *mut c_char,
        password_len: usize,
    ) -> i32;
}

pub(super) fn start() -> i32 {
    // SAFETY: C API takes no pointers and is owned by the Wi-Fi task.
    unsafe { ha_wifi_start() }
}

pub(super) fn scan_start() -> i32 {
    // SAFETY: C API takes no pointers and is owned by the Wi-Fi task.
    unsafe { ha_wifi_scan_start() }
}

pub(super) fn scan_ready() -> bool {
    // SAFETY: C API takes no pointers.
    unsafe { ha_wifi_scan_ready() != 0 }
}

pub(super) fn scan_clear() {
    // SAFETY: C API takes no pointers.
    unsafe { ha_wifi_scan_clear() }
}

pub(super) fn scan_count() -> i32 {
    // SAFETY: C API takes no pointers.
    unsafe { ha_wifi_scan_count() }
}

pub(super) fn scan_ap(index: i32) -> Option<(String, i8, u8)> {
    let mut ssid = [0 as c_char; 33];
    let mut rssi = 0i32;
    let mut auth = 0i32;
    // SAFETY: buffers are valid for the duration of the call; C writes NUL-terminated SSID.
    if unsafe { ha_wifi_scan_get(index, ssid.as_mut_ptr(), ssid.len(), &mut rssi, &mut auth) } == 0
    {
        return None;
    }
    // SAFETY: successful C call guarantees NUL termination in `ssid`.
    let ssid = unsafe { CStr::from_ptr(ssid.as_ptr()) }
        .to_string_lossy()
        .into_owned();
    (!ssid.is_empty()).then_some((ssid, rssi as i8, auth.clamp(0, 4) as u8))
}

pub(super) fn state() -> u8 {
    // SAFETY: C API takes no pointers; the value is a small V2 state code.
    unsafe { ha_wifi_state().clamp(0, 4) as u8 }
}

pub(super) fn status() -> (bool, String, i8) {
    let mut ssid = [0 as c_char; 33];
    let mut rssi = 0i32;
    // SAFETY: buffers are valid for the duration of the call.
    let connected = unsafe { ha_wifi_status(ssid.as_mut_ptr(), ssid.len(), &mut rssi) != 0 };
    if !connected {
        return (false, String::new(), rssi as i8);
    }
    // SAFETY: successful status call guarantees NUL termination.
    let ssid = unsafe { CStr::from_ptr(ssid.as_ptr()) }
        .to_string_lossy()
        .into_owned();
    (true, ssid, rssi as i8)
}

pub(super) fn connect(ssid: &str, password: &str) -> i32 {
    let (Ok(ssid), Ok(password)) = (CString::new(ssid), CString::new(password)) else {
        return -1;
    };
    // SAFETY: both pointers are valid NUL-terminated strings for the call.
    unsafe { ha_wifi_connect_ssid(ssid.as_ptr(), password.as_ptr()) }
}

pub(super) fn known_add(ssid: &str, password: &str) {
    let (Ok(ssid), Ok(password)) = (CString::new(ssid), CString::new(password)) else {
        return;
    };
    // SAFETY: both pointers are valid NUL-terminated strings for the call.
    unsafe { ha_wifi_known_add(ssid.as_ptr(), password.as_ptr()) };
}

pub(super) fn known_remove(ssid: &str) {
    let Ok(ssid) = CString::new(ssid) else {
        return;
    };
    // SAFETY: pointer is a valid NUL-terminated string for the call.
    unsafe { ha_wifi_known_remove(ssid.as_ptr()) };
}

pub(super) fn known_count() -> i32 {
    // SAFETY: C API takes no pointers.
    unsafe { ha_wifi_known_count() }
}

pub(super) fn known_ssid(index: i32) -> Option<String> {
    let mut ssid = [0 as c_char; 33];
    // SAFETY: SSID buffer is valid; password=null/0 requests SSID only.
    if unsafe {
        ha_wifi_known_get(
            index,
            ssid.as_mut_ptr(),
            ssid.len(),
            std::ptr::null_mut(),
            0,
        )
    } == 0
    {
        return None;
    }
    // SAFETY: successful C call guarantees NUL termination.
    Some(
        unsafe { CStr::from_ptr(ssid.as_ptr()) }
            .to_string_lossy()
            .into_owned(),
    )
}
