#[repr(C)]
#[derive(Clone, Copy)]
struct ZbPocEvent {
    extended_addr: u64,
    short_addr: u16,
    cluster_id: u16,
    endpoint: u8,
    command_id: u8,
    payload_len: u8,
    payload: [u8; 8],
}

extern "C" {
    fn zb_poc_start() -> i32;
    fn zb_poc_poll(event: *mut ZbPocEvent) -> i32;
    fn zb_poc_run();
}

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    let started = unsafe { zb_poc_start() };
    if started != 0 {
        log::error!("zb_poc_start failed: {started}");
        panic!("zigbee stack did not start");
    }
    log::info!("zigbee PoC running; polling events");

    std::thread::Builder::new()
        .name("zigbee-mainloop".into())
        .stack_size(8192)
        .spawn(|| unsafe { zb_poc_run() })
        .expect("mainloop thread");

    loop {
        let mut event = ZbPocEvent {
            extended_addr: 0,
            short_addr: 0,
            cluster_id: 0,
            endpoint: 0,
            command_id: 0,
            payload_len: 0,
            payload: [0; 8],
        };
        if unsafe { zb_poc_poll(&mut event) } != 0 {
            let payload = &event.payload[..usize::from(event.payload_len.min(8))];
            log::info!(
                "event eui64={:016x} short=0x{:04x} ep={} cluster=0x{:04x} cmd=0x{:02x} payload={:02x?}",
                event.extended_addr,
                event.short_addr,
                event.endpoint,
                event.cluster_id,
                event.command_id,
                payload
            );
        }
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
}
