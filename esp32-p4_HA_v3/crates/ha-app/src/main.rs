//! First vertical HA V3 slice on hardware.
//!
//! Composition root: starts Domain, Zigbee service, Wi-Fi, System, Web,
//! Display and Automation. Zigbee frames/commands stay inside
//! `zigbee_service`; business state reaches Domain as canonical facts.
//! No async runtime: every service owns its task and inbox.
//!
//! PROPERTY_STATE is RAM-only. Persistent device/config records are written to
//! MicroDB (`domain` partition) and restored at boot; state is not. The device
//! registry lives in `mstore`.

use std::sync::mpsc::sync_channel;

use ha_display as display;
use ha_zigbee_service as zigbee_service;
use ha_zigbee_service::store;

use ha_domain::canonical::{
    CanonicalCommand, COMMAND_PROPERTY, COMMAND_WIFI_CONNECT, COMMAND_WIFI_SCAN,
};
use ha_domain::{Domain, DomainHandle, FactKind, Filter};

mod domain_tables;
mod intake;
#[cfg(feature = "p4-lock-metrics")]
mod p4_probe;
mod platform;
mod runtime;
mod web_actions;

fn main() {
    esp_idf_svc::sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();

    store::init();

    let storage = match store::DomainStorageProvider::new() {
        Ok(storage) => storage,
        Err(error) => {
            log::error!("domain: storage open failed: {error:?}");
            return;
        }
    };
    let mut domain = match Domain::new_with_storage(64, 8, 64, now_ms, storage) {
        Ok(domain) => domain,
        Err(error) => {
            log::error!("domain: init failed: {error:?}");
            return;
        }
    };
    if let Err(error) = domain_tables::register(&mut domain) {
        log::error!("domain: table init failed: {error:?}");
        return;
    }
    if let Err(error) = domain.restore() {
        log::error!("domain: restore failed: {error:?}");
        return;
    }

    let (display_tx, display_rx) = sync_channel(64);
    let (web_tx, web_rx) = sync_channel(64);
    domain.subscribe_channel(Filter::all(), display_tx);
    let web_filter = Filter {
        kind_mask: FactKind::EntityUpserted.bit()
            | FactKind::EntityRemoved.bit()
            | FactKind::Event.bit(),
        ..Filter::all()
    };
    domain.subscribe_channel(web_filter, web_tx);

    platform::init_netif();

    let started = platform::zigbee_start();
    if started != 0 {
        log::error!("zb_poc_start failed: {started}");
        panic!("zigbee stack did not start");
    }
    zigbee_service::install();
    if let Some(count) = option_env!("HA_FORCE_COMPACT") {
        for _ in 0..count.parse::<u32>().unwrap_or(0) {
            store::compact_for_test();
        }
    }
    let domain = DomainHandle::new(domain);
    let command_rx = intake::start();
    if !ha_web_service::start(domain.clone(), &web_actions::WEB_ACTIONS) {
        log::error!("web server not started");
    }
    let (zigbee, zigbee_state) = zigbee_service::start(domain.clone());
    {
        let command_state = std::sync::Arc::clone(&zigbee_state);
        if let Err(error) = domain.register_command(COMMAND_PROPERTY, move |args| {
            let command = CanonicalCommand::decode(args).ok_or(ha_domain::Error::InvalidSize)?;
            command_state.lock().unwrap().submit(command)
        }) {
            log::error!("domain: command executor registration failed: {error:?}");
        }
        if let Err(error) =
            domain.register_command(COMMAND_WIFI_SCAN, |_args| ha_wifi::request_scan())
        {
            log::error!("domain: wifi scan executor registration failed: {error:?}");
        }
        if let Err(error) =
            domain.register_command(COMMAND_WIFI_CONNECT, ha_wifi::connect_from_args)
        {
            log::error!("domain: wifi connect executor registration failed: {error:?}");
        }
    }
    if !display::start(&domain) {
        log::error!("display not started");
    }
    std::thread::Builder::new()
        .name("zigbee-mainloop".into())
        .stack_size(8192)
        .spawn(platform::zigbee_run)
        .expect("zigbee mainloop thread");
    if let Err(error) = ha_system::start(domain.clone()) {
        log::error!("system: start failed: {error}");
    }
    ha_wifi::start(domain.clone());
    let dispatcher_domain = domain.clone();
    let dispatch = domain.dispatch_handle().expect("domain dispatch handle");
    std::thread::Builder::new()
        .name("dispatcher".into())
        .stack_size(4096)
        .spawn(move || loop {
            dispatch.wait();
            if let Err(error) = dispatcher_domain.dispatch_once() {
                log::error!("domain: dispatcher failed: {error:?}");
            }
        })
        .expect("dispatcher thread");
    // shortcut: display/web drain loops use a 20 ms timeout to keep LVGL ticks
    // alive; the fact inbox itself remains bounded and non-blocking.
    if let Err(error) = ha_automation::start(domain.clone()) {
        log::error!("automation: start failed: {error}");
    }
    #[cfg(feature = "p4-lock-metrics")]
    p4_probe::start(domain.clone());
    log::info!("ha-app ready: device table + journal, waiting for button events");
    runtime::prepare(&domain);
    let commands_domain = domain.clone();
    std::thread::Builder::new()
        .name("commands".into())
        .stack_size(16384)
        .spawn(move || runtime::commands(commands_domain, zigbee, command_rx))
        .expect("commands thread");
    std::thread::Builder::new()
        .name("clients".into())
        .stack_size(16384)
        .spawn(move || runtime::clients(domain, display_rx, web_rx))
        .expect("clients thread");
    loop {
        std::thread::sleep(std::time::Duration::from_secs(3600));
    }
}

fn now_ms() -> u64 {
    platform::now_ms()
}
