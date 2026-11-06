//! ESP-IDF SNTP boundary.
use std::ffi::{c_char, CString};

const SNTP_SERVER: &str = "pool.ntp.org";

unsafe extern "C" {
    fn esp_sntp_setoperatingmode(mode: i32);
    fn esp_sntp_setservername(idx: u8, server: *const c_char);
    fn esp_sntp_init();
}

pub(super) fn start() {
    let server = CString::new(SNTP_SERVER).expect("SNTP server");
    // SAFETY: C API copies the server name and initializes the global SNTP client;
    // the pointer stays valid for the duration of the calls.
    unsafe {
        esp_sntp_setoperatingmode(0);
        esp_sntp_setservername(0, server.as_ptr());
        esp_sntp_init();
    }
    log::info!("system: SNTP started (UTC)");
}
