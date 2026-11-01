//! MicroDB access for the device registry (mstore) and Domain state (domain).
//!
//! Both partitions are found by label through the SDK; no absolute offsets.
//! Each table is opened on boot; a fully erased partition is initialized inside
//! micro-db, while corrupt data is kept for inspection instead of being wiped.

use std::sync::Mutex;

use ha_domain::{Backing, EntityDesc, StorageBackend, StorageProvider};
use micro_db::{FlashError, Geometry, NorFlash, RamFlashStorage, RamStorage, RegionManager};
use zigbee_devices::{DeviceDescriptor, DeviceRecord, InterviewStatus, Registry, SIGNATURE_MAX};

pub const MAX_DEVICES: usize = 32;
const CAPACITY: usize = MAX_DEVICES;
const DEFINITION_VERSION: u16 = 1;

struct CFlash {
    size: unsafe extern "C" fn() -> u32,
    read: unsafe extern "C" fn(u32, *mut u8, usize) -> i32,
    write: unsafe extern "C" fn(u32, *const u8, usize) -> i32,
    erase: unsafe extern "C" fn(u32, usize) -> i32,
}

impl CFlash {
    const MSTORE: Self = Self {
        size: ha_mstore_size,
        read: ha_mstore_read,
        write: ha_mstore_write,
        erase: ha_mstore_erase,
    };
    const DOMAIN: Self = Self {
        size: ha_domain_size,
        read: ha_domain_read,
        write: ha_domain_write,
        erase: ha_domain_erase,
    };
}

extern "C" {
    fn ha_mstore_size() -> u32;
    fn ha_mstore_read(offset: u32, buffer: *mut u8, length: usize) -> i32;
    fn ha_mstore_write(offset: u32, buffer: *const u8, length: usize) -> i32;
    fn ha_mstore_erase(offset: u32, length: usize) -> i32;
    fn ha_domain_size() -> u32;
    fn ha_domain_read(offset: u32, buffer: *mut u8, length: usize) -> i32;
    fn ha_domain_write(offset: u32, buffer: *const u8, length: usize) -> i32;
    fn ha_domain_erase(offset: u32, length: usize) -> i32;
}

impl NorFlash for CFlash {
    fn capacity(&self) -> usize {
        unsafe { (self.size)() as usize }
    }

    fn erase_size(&self) -> usize {
        4096
    }

    fn read(&self, offset: usize, out: &mut [u8]) -> Result<(), FlashError> {
        if unsafe { (self.read)(offset as u32, out.as_mut_ptr(), out.len()) } == 0 {
            Ok(())
        } else {
            Err(FlashError::Io)
        }
    }

    fn write(&mut self, offset: usize, data: &[u8]) -> Result<(), FlashError> {
        if unsafe { (self.write)(offset as u32, data.as_ptr(), data.len()) } == 0 {
            Ok(())
        } else {
            Err(FlashError::Io)
        }
    }

    fn erase(&mut self, offset: usize, len: usize) -> Result<(), FlashError> {
        if unsafe { (self.erase)(offset as u32, len) } == 0 {
            Ok(())
        } else {
            Err(FlashError::Io)
        }
    }
}

/// Production storage provider for Domain tables.
pub struct DomainStorageProvider {
    manager: RegionManager,
}

impl DomainStorageProvider {
    pub fn new() -> Result<Self, ha_domain::Error> {
        Ok(Self {
            manager: RegionManager::new(CFlash::DOMAIN)?,
        })
    }
}

impl StorageProvider for DomainStorageProvider {
    fn open(&mut self, desc: &EntityDesc<'_>) -> Result<StorageBackend, ha_domain::Error> {
        let geometry = Geometry {
            capacity: desc.capacity,
            key_size: desc.key_size,
            payload_size: desc.record_size,
        };
        match desc.backing {
            Backing::Ram => Ok(StorageBackend::new(RamStorage::new(geometry)?)),
            Backing::RamFlash => {
                let key = desc.persist_key.ok_or(ha_domain::Error::InvalidArg)?;
                let view =
                    self.manager
                        .bind(key, desc.capacity, desc.key_size, desc.record_size)?;
                let storage = RamFlashStorage::open(view, geometry, key)?;
                Ok(StorageBackend::new(storage))
            }
            Backing::Flash => Err(ha_domain::Error::InvalidState),
        }
    }
}

static REGISTRY: Mutex<Option<Registry<CFlash>>> = Mutex::new(None);

pub fn init() {
    let flash = CFlash::MSTORE;
    let size = flash.capacity();
    let registry = match Registry::open(flash, CAPACITY) {
        Ok(registry) => registry,
        Err(error) => {
            log::error!("registry: not mounted ({error:?}); data preserved");
            return;
        }
    };
    let (bank, seq) = registry.bank_state();
    log::info!(
        "registry ready: mstore={size} bytes entries={} bank={bank} seq={seq}",
        registry.len()
    );
    *REGISTRY.lock().unwrap() = Some(registry);
}

pub fn lookup(ieee: u64) -> Option<&'static z2m_rs::Profile> {
    let registry = REGISTRY.lock().unwrap();
    registry.as_ref()?.profile_for(ieee).ok().flatten()
}

/// Model name from the interview; `None` before the device is saved.
pub fn model(ieee: u64) -> Option<String> {
    let registry = REGISTRY.lock().unwrap();
    registry.as_ref()?.model(ieee).ok().flatten()
}

pub fn descriptor(ieee: u64) -> Option<DeviceDescriptor> {
    let registry = REGISTRY.lock().unwrap();
    registry.as_ref()?.descriptor_for(ieee).ok().flatten()
}

pub fn save(
    ieee: u64,
    short: u16,
    manufacturer: &str,
    model: &str,
    definition_id: &str,
    descriptor: &DeviceDescriptor,
) {
    let mut signature = [0u8; SIGNATURE_MAX];
    let signature_len = match descriptor.encode(&mut signature) {
        Ok(len) => len,
        Err(error) => {
            log::error!("registry: descriptor encode failed ieee={ieee:016x}: {error:?}");
            return;
        }
    };
    let record = DeviceRecord {
        ieee,
        short,
        interview: InterviewStatus::Complete,
        definition_id,
        manufacturer,
        model,
        definition_version: DEFINITION_VERSION,
        configured_version: 0,
        signature: &signature[..signature_len],
    };
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return;
    };
    match registry.put(&record) {
        Ok(()) => {
            log::info!("registry: saved {definition_id} ieee={ieee:016x} short=0x{short:04x}")
        }
        Err(error) => log::error!("registry save failed: {error:?}"),
    }
}

/// Durable deferred-leave mark; canonical cleanup may proceed even if the
/// immediate leave cannot be sent (the announce path retries it).
pub fn mark_removed(ieee: u64) -> bool {
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return false;
    };
    match registry.mark_removed(ieee) {
        Ok(()) => {
            log::info!("registry: removal marked ieee={ieee:016x}");
            true
        }
        Err(error) => {
            log::error!("registry: mark removal failed ieee={ieee:016x}: {error:?}");
            false
        }
    }
}

pub fn is_removed(ieee: u64) -> bool {
    let registry = REGISTRY.lock().unwrap();
    registry
        .as_ref()
        .is_some_and(|registry| registry.is_removed(ieee))
}

pub fn clear_removed(ieee: u64) {
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return;
    };
    match registry.clear_removed(ieee) {
        Ok(()) => log::info!("registry: removal mark cleared ieee={ieee:016x}"),
        Err(error) => log::error!("registry: clear removal failed ieee={ieee:016x}: {error:?}"),
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
    match registry.put(&updated) {
        Ok(()) => log::info!("registry: configured ieee={ieee:016x} version={version}"),
        Err(error) => log::error!("registry configured_version failed: {error:?}"),
    }
}

pub fn device_addresses() -> ([(u64, u16); CAPACITY], usize) {
    let registry = REGISTRY.lock().unwrap();
    let mut out = [(0, 0); CAPACITY];
    let mut count = 0;
    if let Some(registry) = registry.as_ref() {
        let result = registry.for_each(|slot| {
            if let Ok(record) = slot.record() {
                if record.interview == InterviewStatus::Removed {
                    return;
                }
                if count < CAPACITY {
                    out[count] = (record.ieee, record.short);
                    count += 1;
                }
            }
        });
        if let Err(error) = result {
            log::error!("registry: address iteration failed: {error:?}");
        }
    }
    (out, count)
}

/// Diagnostic hook (kept behind the `HA_FORCE_COMPACT` build flag): moves the
/// journal to the other bank without touching RAM records.
pub fn compact_for_test() {
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return;
    };
    match registry.compact() {
        Ok(()) => {
            let (bank, seq) = registry.bank_state();
            log::info!("micro-db: forced compaction bank={bank} seq={seq}");
        }
        Err(error) => log::error!("micro-db: forced compaction failed: {error:?}"),
    }
}
