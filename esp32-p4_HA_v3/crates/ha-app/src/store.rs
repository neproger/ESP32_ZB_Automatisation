//! ESP mstore access + Registry singleton (HA Zigbee Layer).
//!
//! The partition is found by label through the SDK; no absolute offsets. On boot
//! the registry is opened (or formatted once on an unformatted partition).

use std::sync::Mutex;

use micro_db::{FlashError, NorFlash};
use zigbee_devices::{DeviceRecord, InterviewStatus, Registry};

const CAPACITY: usize = 16;
const DEFINITION_VERSION: u16 = 1;

struct EspFlash;

extern "C" {
    fn ha_mstore_size() -> u32;
    fn ha_mstore_read(offset: u32, buffer: *mut u8, length: usize) -> i32;
    fn ha_mstore_write(offset: u32, buffer: *const u8, length: usize) -> i32;
    fn ha_mstore_erase(offset: u32, length: usize) -> i32;
}

impl NorFlash for EspFlash {
    fn capacity(&self) -> usize {
        unsafe { ha_mstore_size() as usize }
    }

    fn read(&mut self, offset: usize, out: &mut [u8]) -> Result<(), FlashError> {
        if unsafe { ha_mstore_read(offset as u32, out.as_mut_ptr(), out.len()) } == 0 {
            Ok(())
        } else {
            Err(FlashError::Io)
        }
    }

    fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashError> {
        if unsafe { ha_mstore_write(offset as u32, data.as_ptr(), data.len()) } == 0 {
            Ok(())
        } else {
            Err(FlashError::Io)
        }
    }

    fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashError> {
        if unsafe { ha_mstore_erase(offset as u32, len) } == 0 {
            Ok(())
        } else {
            Err(FlashError::Io)
        }
    }
}

static REGISTRY: Mutex<Option<Registry>> = Mutex::new(None);

pub fn init() {
    let mut flash = EspFlash;
    let mut header = [0u8; 64];
    if flash.read(0, &mut header).is_err() {
        log::error!("registry: mstore not readable");
        return;
    }
    let erased = header.iter().all(|byte| *byte == 0xFF);

    let registry = match Registry::open(&mut flash, CAPACITY) {
        Ok(registry) => registry,
        // Only a fully erased partition may be initialised. A corrupt header or
        // CRC is a diagnostic condition: keep the data for inspection.
        Err(error) if erased => {
            log::warn!("registry: mstore empty ({error:?}); initialising");
            match Registry::format(&mut flash, CAPACITY) {
                Ok(registry) => registry,
                Err(error) => {
                    log::error!("registry: mstore init failed: {error:?}");
                    return;
                }
            }
        }
        Err(error) => {
            log::error!("registry: not mounted ({error:?}); data preserved");
            return;
        }
    };
    log::info!(
        "registry ready: mstore={} bytes entries={}",
        flash.capacity(),
        registry.len()
    );
    *REGISTRY.lock().unwrap() = Some(registry);
}

pub fn lookup(ieee: u64) -> Option<&'static z2m_rs::Profile> {
    let registry = REGISTRY.lock().unwrap();
    registry.as_ref()?.profile_for(ieee).ok().flatten()
}

pub fn save(ieee: u64, short: u16, manufacturer: &str, model: &str, definition_id: &str) {
    let record = DeviceRecord {
        ieee,
        short,
        interview: InterviewStatus::Complete,
        definition_id,
        manufacturer,
        model,
        definition_version: DEFINITION_VERSION,
        configured_version: 0,
        signature: &[],
    };
    let mut flash = EspFlash;
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return;
    };
    match registry.put(&mut flash, &record) {
        Ok(()) => log::info!(
            "registry: saved {definition_id} ieee={ieee:016x} short=0x{short:04x}"
        ),
        Err(error) => log::error!("registry save failed: {error:?}"),
    }
}

/// A record counts as configured only for the same definition. A profile change
/// (different definition_id) must run the new plan even at the same version.
pub fn is_configured(ieee: u64, definition_id: &str, version: u16) -> bool {
    let registry = REGISTRY.lock().unwrap();
    let Some(registry) = registry.as_ref() else {
        return false;
    };
    registry
        .is_configured(ieee, definition_id, version)
        .unwrap_or(false)
}

pub fn mark_configured(ieee: u64, definition_id: &str, version: u16) {
    let mut flash = EspFlash;
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return;
    };
    let Ok(slot) = registry.slot(ieee) else {
        return;
    };
    let Ok(record) = slot.record() else {
        return;
    };
    let updated = DeviceRecord {
        definition_id,
        configured_version: version,
        ..record
    };
    match registry.put(&mut flash, &updated) {
        Ok(()) => log::info!("registry: configured ieee={ieee:016x} version={version}"),
        Err(error) => log::error!("registry configured_version failed: {error:?}"),
    }
}

pub fn device_addresses() -> ([(u64, u16); CAPACITY], usize) {
    let registry = REGISTRY.lock().unwrap();
    let mut out = [(0, 0); CAPACITY];
    let mut count = 0;
    if let Some(registry) = registry.as_ref() {
        for slot in registry.values() {
            if let Ok(record) = slot.record() {
                if count < CAPACITY {
                    out[count] = (record.ieee, record.short);
                    count += 1;
                }
            }
        }
    }
    (out, count)
}