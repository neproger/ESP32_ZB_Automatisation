//! System service: SNTP, time properties, MINUTE/HALF_HOUR/HOUR/DAY ticks,
//! GeoIP/location and weather. Network readiness comes only from the canonical
//! `WIFI_STATUS` Domain entity; this crate never calls Wi-Fi APIs.

use std::sync::mpsc::{sync_channel, Receiver};
use std::time::Duration;

use ha_domain::canonical::{
    LocationRecord, StateKey, StateRecord, WifiStatusRecord, ENTITY_ENTITY, ENTITY_LOCATION,
    ENTITY_PROPERTY_STATE, ENTITY_SYSTEM, ENTITY_WIFI_STATUS, EVENT_DAY_TICK, EVENT_HALF_HOUR_TICK,
    EVENT_HOUR_TICK, EVENT_MINUTE_TICK, LOCATION_RECORD_LEN, PROPERTY_SYSTEM_HOUR,
    PROPERTY_SYSTEM_MINUTE, PROPERTY_SYSTEM_MINUTES_OF_DAY, PROPERTY_SYSTEM_TIME_STATUS,
    PROPERTY_SYSTEM_TIME_UTC, PROPERTY_SYSTEM_TZ_OFFSET, PROPERTY_SYSTEM_WEEKDAY,
    PROPERTY_SYSTEM_WEEKDAY_MASK, SOURCE_SYSTEM, SYSTEM_DEVICE_UID, WIFI_DEVICE_UID,
    WIFI_STATUS_RECORD_LEN,
};
use ha_domain::{DomainHandle, Fact, FactMeta, FactTarget, Filter, Value};

const STATUS_INBOX: usize = 8;
const TICK_PERIOD: Duration = Duration::from_secs(1);

#[cfg(target_os = "espidf")]
mod geo;
#[cfg(target_os = "espidf")]
const SNTP_SERVER: &str = "pool.ntp.org";

fn wifi_connected(domain: &DomainHandle) -> bool {
    let key = WIFI_DEVICE_UID.to_le_bytes();
    let mut bytes = [0u8; WIFI_STATUS_RECORD_LEN];
    domain
        .entity_get(ENTITY_WIFI_STATUS, &key, &mut bytes)
        .ok()
        .and_then(|_| WifiStatusRecord::decode(&bytes))
        .is_some_and(|record| record.connected != 0)
}

#[cfg(target_os = "espidf")]
fn start_sntp() {
    use std::ffi::{c_char, CString};

    unsafe extern "C" {
        fn esp_sntp_setoperatingmode(mode: i32);
        fn esp_sntp_setservername(idx: u8, server: *const c_char);
        fn esp_sntp_init();
    }

    let server = CString::new(SNTP_SERVER).expect("SNTP server");
    unsafe {
        esp_sntp_setoperatingmode(0);
        esp_sntp_setservername(0, server.as_ptr());
        esp_sntp_init();
    }
    log::info!("system: SNTP started (UTC)");
}

#[cfg(not(target_os = "espidf"))]
fn start_sntp() {
    log::info!("system: SNTP host stub");
}

fn run(domain: DomainHandle, status: Receiver<Fact>, geo_ready: Receiver<()>) {
    loop {
        match status.recv_timeout(Duration::from_secs(1)) {
            Ok(_) | Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
                if wifi_connected(&domain) {
                    start_sntp();
                    break;
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return,
        }
    }
    let _ = geo_ready.recv();
    run_clock(domain);
}

fn run_clock(domain: DomainHandle) {
    let mut last = None;
    loop {
        if let Some((minutes, weekday)) = local_minute(&domain) {
            if last != Some((minutes, weekday)) {
                if last.is_none() {
                    log::info!("system: local clock ready");
                }
                last = Some((minutes, weekday));
                publish_time(&domain, minutes, weekday);
                emit_ticks(&domain, minutes);
            }
        }
        std::thread::sleep(TICK_PERIOD);
    }
}

fn local_minute(domain: &DomainHandle) -> Option<(u16, u8)> {
    let seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .ok()?
        .as_secs();
    if seconds < 1_600_000_000 {
        return None;
    }
    let mut bytes = [0u8; LOCATION_RECORD_LEN];
    let offset = domain
        .entity_get(
            ENTITY_LOCATION,
            &SYSTEM_DEVICE_UID.to_le_bytes(),
            &mut bytes,
        )
        .ok()
        .and_then(|()| LocationRecord::decode(&bytes))
        .map(|location| location.effective_tz_offset_min())
        .unwrap_or(0);
    local_parts(seconds, i32::from(offset))
}

fn local_parts(seconds: u64, offset_min: i32) -> Option<(u16, u8)> {
    let local = i64::try_from(seconds).ok()? + i64::from(offset_min) * 60;
    if local < 0 {
        return None;
    }
    let local = local as u64;
    let weekday = ((local / 86_400 + 3) % 7) as u8;
    let minutes = ((local % 86_400) / 60) as u16;
    Some((minutes, weekday))
}

fn publish_time(domain: &DomainHandle, minutes: u16, weekday: u8) {
    let seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_secs() as u32);
    let mut bytes = [0u8; LOCATION_RECORD_LEN];
    let offset = domain
        .entity_get(
            ENTITY_LOCATION,
            &SYSTEM_DEVICE_UID.to_le_bytes(),
            &mut bytes,
        )
        .ok()
        .and_then(|()| LocationRecord::decode(&bytes))
        .map(|location| i32::from(location.effective_tz_offset_min()))
        .unwrap_or(0);
    let entries = [
        (PROPERTY_SYSTEM_TIME_UTC, Value::u32(seconds)),
        (PROPERTY_SYSTEM_TIME_STATUS, Value::u32(2)),
        (PROPERTY_SYSTEM_HOUR, Value::u32(u32::from(minutes / 60))),
        (PROPERTY_SYSTEM_MINUTE, Value::u32(u32::from(minutes % 60))),
        (PROPERTY_SYSTEM_WEEKDAY, Value::u32(u32::from(weekday))),
        (PROPERTY_SYSTEM_WEEKDAY_MASK, Value::u32(1u32 << weekday)),
        (
            PROPERTY_SYSTEM_MINUTES_OF_DAY,
            Value::u32(u32::from(minutes)),
        ),
        (PROPERTY_SYSTEM_TZ_OFFSET, Value::i32(offset)),
    ];
    let meta = FactMeta {
        source: SOURCE_SYSTEM as u8,
        value: Value::NONE,
    };
    for (property, value) in entries {
        let key = StateKey {
            entity: ENTITY_SYSTEM,
            property,
        }
        .to_bytes();
        let record = StateRecord { value }.encode();
        let _ = domain.entity_put(ENTITY_PROPERTY_STATE, &key, &record, meta);
    }
}

fn emit_ticks(domain: &DomainHandle, minutes: u16) {
    publish_event(domain, EVENT_MINUTE_TICK);
    if minutes % 30 == 0 {
        publish_event(domain, EVENT_HALF_HOUR_TICK);
    }
    if minutes % 60 == 0 {
        publish_event(domain, EVENT_HOUR_TICK);
        if minutes == 0 {
            publish_event(domain, EVENT_DAY_TICK);
        }
    }
}

fn publish_event(domain: &DomainHandle, event: u32) {
    let key = ENTITY_SYSTEM.to_le_bytes();
    let payload = event.to_le_bytes();
    if let Err(error) = domain.payload_put(
        Some(FactTarget {
            entity: ENTITY_ENTITY,
            key: &key,
        }),
        FactMeta {
            source: SOURCE_SYSTEM as u8,
            value: Value::u32(event),
        },
        &payload,
    ) {
        log::error!("system: event {event} failed: {error:?}");
    }
}

/// Wait for the canonical Wi-Fi status to report a connected link, then start
/// SNTP. `ha-app` owns startup order and error logging.
pub fn start(domain: DomainHandle) -> Result<(), String> {
    let (sender, receiver) = sync_channel(STATUS_INBOX);
    domain
        .subscribe_channel(Filter::entity(ENTITY_WIFI_STATUS), sender)
        .map_err(|error| format!("system status subscribe: {error:?}"))?;
    let (geo_ready_tx, geo_ready_rx) = sync_channel(1);
    #[cfg(target_os = "espidf")]
    geo::start(domain.clone(), geo_ready_tx);
    #[cfg(not(target_os = "espidf"))]
    drop(geo_ready_tx);
    std::thread::Builder::new()
        .name("system-sntp".into())
        .stack_size(4096)
        .spawn(move || run(domain, receiver, geo_ready_rx))
        .map_err(|error| format!("system SNTP task: {error:?}"))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use ha_domain::canonical::{SOURCE_SYSTEM, WIFI_STATUS_RECORD_LEN};
    use ha_domain::{Backing, Domain, DomainHandle, EntityDesc, Value};

    fn domain_with_status() -> DomainHandle {
        let mut domain = Domain::new(8, 4, 32).unwrap();
        domain
            .register_entity(EntityDesc {
                entity: ENTITY_WIFI_STATUS,
                key_size: 8,
                record_size: WIFI_STATUS_RECORD_LEN,
                capacity: 1,
                backing: Backing::Ram,
                persist_key: None,
            })
            .unwrap();
        DomainHandle::new(domain)
    }

    fn put_status(domain: &DomainHandle, connected: bool) {
        let record = WifiStatusRecord {
            state: if connected { 3 } else { 0 },
            connected: connected as u8,
            rssi: -40,
            ssid: [0u8; 32],
        }
        .encode();
        domain
            .entity_put(
                ENTITY_WIFI_STATUS,
                &WIFI_DEVICE_UID.to_le_bytes(),
                &record,
                ha_domain::FactMeta {
                    source: SOURCE_SYSTEM as u8,
                    value: Value::NONE,
                },
            )
            .unwrap();
    }

    #[test]
    fn wifi_status_is_the_contract() {
        let domain = domain_with_status();
        assert!(!wifi_connected(&domain));
        put_status(&domain, true);
        assert!(wifi_connected(&domain));
    }

    #[test]
    fn local_minutes_and_weekday_follow_v2() {
        assert_eq!(local_parts(0, 0), Some((0, 3)));
        assert_eq!(local_parts(86_400, 0), Some((0, 4)));
        assert_eq!(local_parts(3_600 + 61, 180), Some((4 * 60 + 1, 3)));
    }
}
