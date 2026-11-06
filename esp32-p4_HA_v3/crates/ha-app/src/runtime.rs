#![forbid(unsafe_code)]

//! Runtime loop and Domain command handling.
//!
//! Owns the service inboxes and business command path so `main` stays a
//! composition root. P4 drain shortcuts are intentionally kept here.

use std::sync::mpsc::Receiver;
use std::time::Duration;

use ha_domain::canonical::{
    CanonicalCommand, LocationRecord, SettingsRecord, COMMAND_PROPERTY, ENTITY_ENTITY,
    ENTITY_LOCATION, ENTITY_SETTINGS, LOCATION_RECORD_LEN, SETTINGS_ID, SETTINGS_RECORD_LEN,
    SOURCE_SYSTEM, SOURCE_UI, SOURCE_ZIGBEE, SYSTEM_DEVICE_UID,
};
use ha_domain::{DomainHandle, DomainTransaction, Fact, FactMeta};

use ha_web_service as web;

use ha_display as display;

use ha_zigbee_service as zigbee_service;
use ha_zigbee_service::take_capabilities;

use ha_config as config;

use crate::intake;
fn handle_intake(domain: &DomainHandle, zigbee: &zigbee_service::Handle, request: intake::Request) {
    match request {
        intake::Request::Command {
            entity,
            property,
            action,
            value,
        } => {
            let command = CanonicalCommand {
                entity,
                property,
                action,
                value,
            };
            if let Err(error) = post_command(domain, command, SOURCE_UI as u8) {
                log::warn!(
                    "command rejected: entity={entity:016x} property={property} error={error:?}"
                );
            }
        }
        intake::Request::RenameEntity { entity, name } => {
            if let Err(error) = domain.transaction(|transaction| {
                let key = entity.to_le_bytes();
                let mut current = [0u8; ha_domain::canonical::ENTITY_RECORD_LEN];
                transaction.entity_get(ENTITY_ENTITY, &key, &mut current)?;
                let device = u64::from_le_bytes(current[..8].try_into().expect("entity owner"));
                let encoded = ha_domain::canonical::EntityRecord::new(device, &name).encode();
                transaction
                    .entity_put(
                        ENTITY_ENTITY,
                        &key,
                        &encoded,
                        FactMeta {
                            source: SOURCE_UI as u8,
                            value: ha_domain::Value::NONE,
                        },
                    )
                    .map(|_| ())
            }) {
                log::warn!("domain: entity rename failed: {error:?}");
            }
        }
        intake::Request::RenameDevice { device, name } => {
            if let Err(error) = domain.transaction(|transaction| {
                ha_device::rename(transaction, SOURCE_UI as u8, device, &name).map(|_| ())
            }) {
                log::warn!("domain: device rename failed: {error:?}");
            }
        }
        intake::Request::RemoveDevice { device } => {
            if let Err(error) = zigbee.remove_device(device) {
                log::warn!("device: remove {device:016x} rejected: {error:?}");
            }
        }
        intake::Request::Upsert {
            entity,
            key,
            record,
        } => {
            let result = if entity == ENTITY_LOCATION {
                let meta = FactMeta {
                    source: SOURCE_UI as u8,
                    value: ha_domain::Value::NONE,
                };
                domain.entity_put(entity, &key, &record, meta).map(|_| ())
            } else {
                config::put_record(domain, entity, &key, &record).map(|_| ())
            };
            if let Err(error) = result {
                log::warn!("domain: upsert entity={entity} failed: {error:?}");
            }
        }
        intake::Request::Remove { entity, key } => {
            let id = || {
                key.get(..8)
                    .and_then(|bytes| bytes.try_into().ok())
                    .map(u64::from_le_bytes)
                    .ok_or(ha_domain::Error::InvalidSize)
            };
            match entity {
                ha_domain::canonical::ENTITY_GROUP => {
                    match id().and_then(|id| config::remove_group(domain, id)) {
                        Ok(count) => log::info!("config: removed group records={count}"),
                        Err(error) => log::warn!("config: group removal failed: {error:?}"),
                    }
                }
                ha_domain::canonical::ENTITY_AUTOMATION => {
                    match id().and_then(|id| config::remove_automation(domain, id)) {
                        Ok(count) => log::info!("config: removed automation records={count}"),
                        Err(error) => log::warn!("config: automation removal failed: {error:?}"),
                    }
                }
                _ => {
                    if let Err(error) = config::remove_record(domain, entity, &key) {
                        log::warn!("domain: remove entity={entity} failed: {error:?}");
                    }
                }
            }
        }
    }
}

fn apply_capabilities(
    domain: &mut DomainTransaction<'_>,
    entity: u64,
    capabilities: ha_domain::canonical::CapabilitiesRecord,
) -> Result<bool, ha_domain::Error> {
    ha_device::set_capabilities(domain, SOURCE_ZIGBEE as u8, entity, &capabilities)
}

fn post_command(
    domain: &DomainHandle,
    command: CanonicalCommand,
    source: u8,
) -> Result<(), ha_domain::Error> {
    let args = command.encode();
    let meta = FactMeta {
        source,
        value: ha_domain::Value::NONE,
    };
    domain.post(COMMAND_PROPERTY, &args, None, meta)
}
fn ensure_system_location(domain: &DomainHandle) {
    let key = SYSTEM_DEVICE_UID.to_le_bytes();
    let mut current = [0u8; LOCATION_RECORD_LEN];
    if domain
        .entity_get(ENTITY_LOCATION, &key, &mut current)
        .is_ok()
    {
        return;
    }
    let record = LocationRecord::default().encode();
    let meta = FactMeta {
        source: SOURCE_SYSTEM as u8,
        value: ha_domain::Value::NONE,
    };
    match domain.entity_put(ENTITY_LOCATION, &key, &record, meta) {
        Ok(_) => log::info!("domain: location seeded"),
        Err(error) => log::warn!("domain: location seed failed: {error:?}"),
    }
}

fn ensure_settings(domain: &DomainHandle) {
    let key = [SETTINGS_ID];
    let mut bytes = [0u8; SETTINGS_RECORD_LEN];
    if domain.entity_get(ENTITY_SETTINGS, &key, &mut bytes).is_ok() {
        return;
    }
    let record = SettingsRecord::default().encode();
    match config::put_record(domain, ENTITY_SETTINGS, &key, &record) {
        Ok(_) => log::info!("domain: settings seeded"),
        Err(error) => log::warn!("domain: settings seed failed: {error:?}"),
    }
}

pub fn prepare(domain: &DomainHandle) {
    ensure_system_location(domain);
    ensure_settings(domain);
}

pub fn commands(
    domain: DomainHandle,
    zigbee: zigbee_service::Handle,
    receiver: Receiver<intake::Request>,
) -> ! {
    loop {
        if let Ok(request) = receiver.recv() {
            handle_intake(&domain, &zigbee, request);
        }
    }
}

pub fn clients(domain: DomainHandle, display_rx: Receiver<Fact>, web_rx: Receiver<Fact>) -> ! {
    let mut last_subscriber_drops = 0u64;
    loop {
        while let Some(nav) = display::take_nav() {
            match nav {
                display::Nav::WifiScan => {
                    if let Err(error) = display::request_wifi_scan() {
                        log::warn!("wifi: scan request rejected ({error:?})");
                    }
                }
                display::Nav::OpenWifi => display::navigate(&domain, display::Nav::OpenWifi),
                _ => display::navigate(&domain, nav),
            }
        }
        while let Some((entity, capabilities)) = take_capabilities() {
            if let Err(error) = domain.transaction(|transaction| {
                apply_capabilities(transaction, entity, capabilities).map(|_| ())
            }) {
                log::warn!("domain: capabilities update failed: {error:?}");
            }
        }
        display::drain(&domain, &display_rx);
        display::tick();
        web::drain(&domain, &web_rx);
        web::flush();
        let subscriber_drops = domain.subscriber_drops().unwrap_or(0);
        if subscriber_drops != last_subscriber_drops {
            log::warn!("domain: subscriber drops={subscriber_drops}");
            last_subscriber_drops = subscriber_drops;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}
