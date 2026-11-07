//! ESP-IDF SNTP boundary.
use std::ffi::c_char;

/// SNTP keeps this pointer after init, so it must live for the whole program.
const SNTP_SERVER: &[u8] = b"pool.ntp.org\0";

unsafe extern "C" {
    fn esp_sntp_setoperatingmode(mode: i32);
    fn esp_sntp_setservername(idx: u8, server: *const c_char);
    fn esp_sntp_init();
}

pub(super) fn start() {
    // SAFETY: the server name is a static NUL-terminated string lifetime; the
    // SNTP client stores the pointer and copies nothing.
    unsafe {
        esp_sntp_setoperatingmode(0);
        esp_sntp_setservername(0, SNTP_SERVER.as_ptr() as *const c_char);
        esp_sntp_init();
    }
    log::info!("system: SNTP started (UTC)");
}
