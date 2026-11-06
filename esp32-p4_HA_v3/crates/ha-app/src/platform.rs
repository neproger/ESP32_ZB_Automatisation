//! ESP-IDF platform bootstrap and Zigbee C entry points.
extern "C" {
    fn zb_poc_start() -> i32;
    fn zb_poc_run();
    fn zb_poc_open_network(seconds: u8) -> i32;
}

pub(super) fn init_netif() {
    // SAFETY: called once from the composition root before services start.
    unsafe {
        esp_idf_svc::sys::esp_netif_init();
        esp_idf_svc::sys::esp_event_loop_create_default();
    }
}

pub(super) fn zigbee_start() -> i32 {
    // SAFETY: starts the single global Zigbee stack; called once at boot.
    unsafe { zb_poc_start() }
}

pub(super) fn zigbee_run() {
    // SAFETY: runs the global Zigbee stack on its dedicated thread.
    unsafe { zb_poc_run() }
}

pub(super) fn zigbee_open_network(seconds: u8) -> i32 {
    // SAFETY: forwards the UI discovery window to the global Zigbee stack.
    unsafe { zb_poc_open_network(seconds) }
}

pub(super) fn now_ms() -> u64 {
    // SAFETY: esp_timer_get_time is a pure IDF accessor.
    (unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000).max(0) as u64
}
