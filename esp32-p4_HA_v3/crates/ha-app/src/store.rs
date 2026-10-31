//! MicroDB access for the device registry (mstore) and Domain state (domain).
//!
//! Both partitions are found by label through the SDK; no absolute offsets. On
//! boot each table is opened, or formatted once when the partition is fully
//! erased. A corrupt table is kept for inspection instead of being wiped.

use std::sync::Mutex;

use ha_domain::{Domain, FactMeta, Value};
use micro_db::{Fixed, FlashError, NorFlash, PersistedTable};
use zigbee_devices::{DeviceDescriptor, DeviceRecord, InterviewStatus, Registry, SIGNATURE_MAX};

pub const MAX_DEVICES: usize = 32;
const CAPACITY: usize = MAX_DEVICES;
const DEFINITION_VERSION: u16 = 1;

const DOMAIN_KEY_MAX: usize = ha_domain::canonical::STATE_KEY_LEN;
const DOMAIN_RECORD_MAX: usize = ha_domain::canonical::AUTOMATION_RECORD_LEN;
const DOMAIN_CAPACITY: usize = 384;
const DOMAIN_PERSIST_ID: &str = "ha_domain";

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

/// One persisted Domain record: entity type + canonical key + record bytes.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct DomainKey {
    entity: u32,
    key_len: u8,
    key: [u8; DOMAIN_KEY_MAX],
}

impl Fixed for DomainKey {
    const SIZE: usize = 4 + 1 + DOMAIN_KEY_MAX;

    fn to_bytes(&self, out: &mut [u8]) {
        out[..4].copy_from_slice(&self.entity.to_le_bytes());
        out[4] = self.key_len;
        out[5..5 + DOMAIN_KEY_MAX].copy_from_slice(&self.key);
    }

    fn from_bytes(bytes: &[u8]) -> Self {
        let mut key = [0u8; DOMAIN_KEY_MAX];
        key.copy_from_slice(&bytes[5..5 + DOMAIN_KEY_MAX]);
        Self {
            entity: u32::from_le_bytes(bytes[0..4].try_into().expect("domain key entity")),
            key_len: bytes[4].min(DOMAIN_KEY_MAX as u8),
            key,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct DomainSlot {
    len: u16,
    bytes: [u8; DOMAIN_RECORD_MAX],
}

impl Fixed for DomainSlot {
    const SIZE: usize = 2 + DOMAIN_RECORD_MAX;

    fn to_bytes(&self, out: &mut [u8]) {
        out[..2].copy_from_slice(&self.len.to_le_bytes());
        out[2..2 + DOMAIN_RECORD_MAX].copy_from_slice(&self.bytes);
    }

    fn from_bytes(bytes: &[u8]) -> Self {
        let mut record = [0u8; DOMAIN_RECORD_MAX];
        record.copy_from_slice(&bytes[2..2 + DOMAIN_RECORD_MAX]);
        Self {
            len: u16::from_le_bytes(bytes[0..2].try_into().expect("domain slot len")),
            bytes: record,
        }
    }
}

type DomainTable = PersistedTable<DomainKey, DomainSlot>;

static REGISTRY: Mutex<Option<Registry>> = Mutex::new(None);
static DOMAIN_DB: Mutex<Option<DomainTable>> = Mutex::new(None);

pub fn init() {
    let mut flash = CFlash::MSTORE;
    let size = flash.capacity();
    let mut header = [0u8; 64];
    if flash.read(0, &mut header).is_err() {
        log::error!("registry: mstore not readable");
        return;
    }
    // With two banks either header can be the active one; only a fully erased
    // region may be formatted.
    let mut standby_header = [0u8; 64];
    let standby_erased = flash
        .read(size / 2, &mut standby_header)
        .map(|()| standby_header.iter().all(|byte| *byte == 0xFF))
        .unwrap_or(false);
    let erased = header.iter().all(|byte| *byte == 0xFF) && standby_erased;

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
    let (bank, seq) = registry.bank_state();
    log::info!(
        "registry ready: mstore={} bytes entries={} bank={bank} seq={seq}",
        flash.capacity(),
        registry.len()
    );
    *REGISTRY.lock().unwrap() = Some(registry);
    *DOMAIN_DB.lock().unwrap() = init_domain();
}

fn init_domain() -> Option<DomainTable> {
    let mut flash = CFlash::DOMAIN;
    let size = flash.capacity();
    if size == 0 {
        log::error!("domain: partition not found");
        return None;
    }
    let mut first = [0u8; 64];
    let mut second = [0u8; 64];
    let erased = flash.read(0, &mut first).is_ok()
        && first.iter().all(|byte| *byte == 0xFF)
        && flash.read(size / 2, &mut second).is_ok()
        && second.iter().all(|byte| *byte == 0xFF);
    match DomainTable::open(&mut flash, DOMAIN_CAPACITY, DOMAIN_PERSIST_ID) {
        Ok(table) => {
            let (bank, seq) = table.bank_state();
            log::info!(
                "domain ready: domain={size} bytes entries={} bank={bank} seq={seq}",
                table.len()
            );
            Some(table)
        }
        Err(micro_db::Error::InvalidSize) => {
            log::warn!("domain: schema changed; formatting");
            match DomainTable::format(&mut flash, DOMAIN_CAPACITY, DOMAIN_PERSIST_ID) {
                Ok(table) => Some(table),
                Err(error) => {
                    log::error!("domain: format after schema change failed: {error:?}");
                    None
                }
            }
        }
        Err(error) if erased => {
            log::warn!("domain: empty ({error:?}); initialising");
            match DomainTable::format(&mut flash, DOMAIN_CAPACITY, DOMAIN_PERSIST_ID) {
                Ok(table) => Some(table),
                Err(error) => {
                    log::error!("domain: init failed: {error:?}");
                    None
                }
            }
        }
        Err(error) => {
            log::error!("domain: not mounted ({error:?}); data preserved");
            None
        }
    }
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
    let mut flash = CFlash::MSTORE;
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return;
    };
    match registry.put(&mut flash, &record) {
        Ok(()) => {
            log::info!("registry: saved {definition_id} ieee={ieee:016x} short=0x{short:04x}")
        }
        Err(error) => log::error!("registry save failed: {error:?}"),
    }
}

/// Durable deferred-leave mark; canonical cleanup may proceed even if the
/// immediate leave cannot be sent (the announce path retries it).
pub fn mark_removed(ieee: u64) -> bool {
    let mut flash = CFlash::MSTORE;
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return false;
    };
    match registry.mark_removed(&mut flash, ieee) {
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
    let mut flash = CFlash::MSTORE;
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return;
    };
    match registry.clear_removed(&mut flash, ieee) {
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
    let mut flash = CFlash::MSTORE;
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
                if record.interview == InterviewStatus::Removed {
                    continue;
                }
                if count < CAPACITY {
                    out[count] = (record.ieee, record.short);
                    count += 1;
                }
            }
        }
    }
    (out, count)
}

/// Persist one canonical Domain record. Called from the Domain adapter sink.
pub fn save_domain(entity: u32, key: &[u8], record: &[u8]) {
    if key.len() > DOMAIN_KEY_MAX || record.len() > DOMAIN_RECORD_MAX {
        log::error!(
            "domain: persist size entity={entity} key={} record={}",
            key.len(),
            record.len()
        );
        return;
    }
    let mut guard = DOMAIN_DB.lock().unwrap();
    let Some(table) = guard.as_mut() else {
        return;
    };
    let mut key_slot = DomainKey {
        entity,
        key_len: key.len() as u8,
        key: [0; DOMAIN_KEY_MAX],
    };
    key_slot.key[..key.len()].copy_from_slice(key);
    let mut record_slot = DomainSlot {
        len: record.len() as u16,
        bytes: [0; DOMAIN_RECORD_MAX],
    };
    record_slot.bytes[..record.len()].copy_from_slice(record);
    let mut flash = CFlash::DOMAIN;
    if let Err(error) = table.put(&mut flash, key_slot, record_slot) {
        log::error!("domain: persist entity={entity} failed: {error:?}");
    }
}

pub fn remove_domain(entity: u32, key: &[u8]) {
    if key.len() > DOMAIN_KEY_MAX {
        return;
    }
    let mut guard = DOMAIN_DB.lock().unwrap();
    let Some(table) = guard.as_mut() else {
        return;
    };
    let mut key_slot = DomainKey {
        entity,
        key_len: key.len() as u8,
        key: [0; DOMAIN_KEY_MAX],
    };
    key_slot.key[..key.len()].copy_from_slice(key);
    let mut flash = CFlash::DOMAIN;
    if let Err(error) = table.remove(&mut flash, &key_slot) {
        log::error!("domain: remove entity={entity} failed: {error:?}");
    }
}

/// Load persistent device/config records back into RAM and mirror them to the UI.
///
/// PROPERTY_STATE is deliberately not restored: it is RAM truth and devices
/// report it again after boot. Nothing is written to or cleaned from flash for
/// state here.
pub fn restore_domain(domain: &mut Domain) {
    let guard = DOMAIN_DB.lock().unwrap();
    let Some(table) = guard.as_ref() else {
        return;
    };

    let mut restored = 0;
    let mut skipped = 0;
    // shortcut: no schema migration; an incompatible record is skipped and
    // rewritten on the next change.
    for view in table.iter() {
        if view.key.entity == ha_domain::canonical::ENTITY_PROPERTY_STATE {
            continue;
        }
        let key_len = usize::from(view.key.key_len).min(DOMAIN_KEY_MAX);
        let key = &view.key.key[..key_len];
        let record_len = usize::from(view.payload.len).min(DOMAIN_RECORD_MAX);
        let record = &view.payload.bytes[..record_len];
        let meta = FactMeta {
            source: ha_domain::canonical::SOURCE_ZIGBEE as u8,
            value: Value::NONE,
        };
        match domain.entity_put(view.key.entity, key, record, meta) {
            Ok(_) => {
                if let Ok(frame_type) = u8::try_from(view.key.entity) {
                    crate::web::upsert(frame_type, key, record);
                }
                restored += 1;
            }
            Err(error) => {
                skipped += 1;
                log::warn!(
                    "domain: restore entity={} skipped: {error:?}",
                    view.key.entity
                );
            }
        }
    }
    log::info!("domain: restored {restored} records ({skipped} skipped)");
}

/// Diagnostic hook (kept behind the `HA_FORCE_COMPACT` build flag): moves the
/// journal to the other bank without touching RAM records.
pub fn compact_for_test() {
    let mut flash = CFlash::MSTORE;
    let mut guard = REGISTRY.lock().unwrap();
    let Some(registry) = guard.as_mut() else {
        return;
    };
    match registry.compact(&mut flash) {
        Ok(()) => {
            let (bank, seq) = registry.bank_state();
            log::info!("micro-db: forced compaction bank={bank} seq={seq}");
        }
        Err(error) => log::error!("micro-db: forced compaction failed: {error:?}"),
    }
}
