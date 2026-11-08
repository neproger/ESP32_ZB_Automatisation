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

use ha_display as display;
use ha_zigbee_service as zigbee_service;
use ha_zigbee_service::store;

use ha_domain::{Domain, DomainHandle};

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
    let mut domain = match Domain::new_with_storage(64, 8, 64, ha_platform::now_ms, storage) {
        Ok(domain) => domain,
        Err(error) => {
            log::error!("domain: init failed: {error:?}");
            return;
        }
    };
    if let Err(error) = ha_schema::register(&mut domain) {
        log::error!("domain: table init failed: {error:?}");
        return;
    }
    if let Err(error) = domain.restore() {
        log::error!("domain: restore failed: {error:?}");
        return;
    }

    ha_platform::init_netif();

    if let Some(count) = option_env!("HA_FORCE_COMPACT") {
        for _ in 0..count.parse::<u32>().unwrap_or(0) {
            store::compact_for_test();
        }
    }
    let domain = DomainHandle::new(domain);
    zigbee_service::start(domain.clone());
    ha_wifi::start(domain.clone());
    let web_domain = domain.clone();
    std::thread::Builder::new()
        .name("web-start".into())
        .stack_size(16384)
        .spawn(move || {
            if !ha_wifi::wait_connected(std::time::Duration::from_secs(60)) {
                log::warn!("web: Wi-Fi not connected; starting anyway");
            }
            let actions = ha_web_actions::WebActions::new(web_domain.clone());
            if !ha_web_service::start(web_domain, actions) {
                log::error!("web server not started");
            }
        })
        .expect("web start thread");
    if !display::start(&domain) {
        log::error!("display not started");
    }
    if let Err(error) = ha_system::start(domain.clone()) {
        log::error!("system: start failed: {error}");
    }
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
    if let Err(error) = ha_automation::start(domain.clone()) {
        log::error!("automation: start failed: {error}");
    }
    #[cfg(feature = "p4-lock-metrics")]
    ha_platform::p4_probe::start(domain.clone());
    log::info!("ha-app ready: device table + journal, waiting for button events");
    ha_config::seed_defaults(&domain);
    loop {
        std::thread::sleep(std::time::Duration::from_secs(3600));
    }
}
