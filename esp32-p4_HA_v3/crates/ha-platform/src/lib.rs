//! ESP-IDF platform bootstrap shared by the composition root.

#[cfg(feature = "p4-lock-metrics")]
pub mod p4_probe;

pub fn init_netif() {
    // SAFETY: called once from the composition root before services start.
    unsafe {
        esp_idf_svc::sys::esp_netif_init();
        esp_idf_svc::sys::esp_event_loop_create_default();
    }
}

pub fn now_ms() -> u64 {
    // SAFETY: esp_timer_get_time is a pure IDF accessor.
    (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000).max(0) as u64
}
