//! Thin Device Registry: EUI64 -> canonical DeviceRecord over PersistedTable.
//!
//! No own storage model, indexes or serializers: the canonical record is encoded
//! by `DeviceRecord`, stored by `micro-db`, and the profile comes from `z2m-rs`.
//! The short address in the record is a cache; callers must revalidate it against
//! the network before sending commands.

use micro_db::{Error, Fixed, NorFlash, PersistedTable};
use z2m_rs::Interview;

use crate::{DeviceDescriptor, DeviceRecord, RecordError, MAX_RECORD};

const PERSIST_ID: &str = "ha_zigbee_devices";
pub const RECORD_SLOT_SIZE: usize = 2 + MAX_RECORD;

/// Fixed-size container for the variable-length canonical record.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct RecordSlot {
    len: u16,
    bytes: [u8; MAX_RECORD],
}

impl RecordSlot {
    pub fn empty() -> Self {
        Self {
            len: 0,
            bytes: [0; MAX_RECORD],
        }
    }

    pub fn record(&self) -> Result<DeviceRecord<'_>, Error> {
        DeviceRecord::parse(&self.bytes[..usize::from(self.len)]).map_err(map_record_error)
    }
}

impl Fixed for RecordSlot {
    const SIZE: usize = RECORD_SLOT_SIZE;

    fn to_bytes(&self, out: &mut [u8]) {
        out[..2].copy_from_slice(&self.len.to_le_bytes());
        out[2..].copy_from_slice(&self.bytes);
    }

    fn from_bytes(bytes: &[u8]) -> Self {
        let len = usize::from(u16::from_le_bytes([bytes[0], bytes[1]])).min(MAX_RECORD);
        let mut slot = Self::empty();
        slot.len = len as u16;
        slot.bytes[..len].copy_from_slice(&bytes[2..2 + len]);
        slot
    }
}

pub struct Registry {
    table: PersistedTable<[u8; 8], RecordSlot>,
}

impl Registry {
    pub fn format<F: NorFlash>(flash: &mut F, capacity: usize) -> Result<Self, Error> {
        Ok(Self {
            table: PersistedTable::format(flash, capacity, PERSIST_ID)?,
        })
    }

    pub fn open<F: NorFlash>(flash: &mut F, capacity: usize) -> Result<Self, Error> {
        Ok(Self {
            table: PersistedTable::open(flash, capacity, PERSIST_ID)?,
        })
    }

    pub fn put<F: NorFlash>(
        &mut self,
        flash: &mut F,
        record: &DeviceRecord<'_>,
    ) -> Result<(), Error> {
        let mut slot = RecordSlot::empty();
        let len = record.encode(&mut slot.bytes).map_err(map_record_error)?;
        slot.len = len as u16;
        self.table
            .put(flash, record.ieee.to_le_bytes(), slot)
            .map(|_| ())
    }

    pub fn slot(&self, ieee: u64) -> Result<RecordSlot, Error> {
        let mut slot = RecordSlot::empty();
        self.table.get(&ieee.to_le_bytes(), &mut slot)?;
        Ok(slot)
    }

    pub fn descriptor_for(&self, ieee: u64) -> Result<Option<DeviceDescriptor>, Error> {
        let slot = self.slot(ieee)?;
        let record = slot.record()?;
        if record.interview == crate::InterviewStatus::Removed || record.signature.is_empty() {
            return Ok(None);
        }
        DeviceDescriptor::decode(record.signature)
            .map(Some)
            .map_err(|_| Error::Corrupt)
    }

    pub fn profile_for(&self, ieee: u64) -> Result<Option<&'static z2m_rs::Profile>, Error> {
        let slot = self.slot(ieee)?;
        let record = slot.record()?;
        if record.interview == crate::InterviewStatus::Removed {
            return Ok(None);
        }
        if record.definition_id == crate::standard::STANDARD_PROFILE_ID {
            return Ok(Some(&crate::standard::STANDARD_PROFILE));
        }
        if let Some(profile) = z2m_rs::profile_by_id(record.definition_id) {
            return Ok(Some(profile));
        }
        // Records written before `definition_id` was resolvable keep working.
        Ok(z2m_rs::identify(&Interview {
            manufacturer: Some(record.manufacturer),
            model: record.model,
        }))
    }

    /// Human-readable model from the interview; `None` when the record has no
    /// model string (the caller falls back to the profile id).
    pub fn model(&self, ieee: u64) -> Result<Option<String>, Error> {
        let slot = self.slot(ieee)?;
        let record = slot.record()?;
        if record.interview == crate::InterviewStatus::Removed {
            return Ok(None);
        }
        Ok((!record.model.is_empty()).then(|| record.model.to_string()))
    }

    /// Durable deferred-leave mark: keep a tombstone instead of deleting the
    /// row, so an announce cannot silently re-add the device.
    pub fn mark_removed<F: NorFlash>(&mut self, flash: &mut F, ieee: u64) -> Result<(), Error> {
        let previous = self.slot(ieee).ok();
        let record = previous.as_ref().and_then(|slot| slot.record().ok());
        let tombstone = DeviceRecord {
            ieee,
            short: 0,
            interview: crate::InterviewStatus::Removed,
            definition_id: "",
            manufacturer: record.map(|record| record.manufacturer).unwrap_or(""),
            model: record.map(|record| record.model).unwrap_or(""),
            definition_version: 0,
            configured_version: 0,
            signature: &[],
        };
        self.put(flash, &tombstone)
    }

    pub fn is_removed(&self, ieee: u64) -> bool {
        let Ok(slot) = self.slot(ieee) else {
            return false;
        };
        slot.record()
            .map(|record| record.interview == crate::InterviewStatus::Removed)
            .unwrap_or(false)
    }

    /// Clears the mark only after the leave succeeded. A row re-written by a
    /// normal interview is left alone.
    pub fn clear_removed<F: NorFlash>(&mut self, flash: &mut F, ieee: u64) -> Result<(), Error> {
        let removed = self.is_removed(ieee);
        if !removed {
            return Ok(());
        }
        self.remove(flash, ieee)
    }

    /// Configuration is bound to the definition: a different profile at the
    /// same version still needs its own plan.
    pub fn is_configured(
        &self,
        ieee: u64,
        definition_id: &str,
        version: u16,
    ) -> Result<bool, Error> {
        let slot = self.slot(ieee)?;
        let record = slot.record()?;
        Ok(record.definition_id == definition_id && record.configured_version >= version)
    }

    pub fn set_configured<F: NorFlash>(
        &mut self,
        flash: &mut F,
        ieee: u64,
        version: u16,
    ) -> Result<(), Error> {
        let slot = self.slot(ieee)?;
        let record = slot.record()?;
        let updated = DeviceRecord {
            configured_version: version,
            ..record
        };
        self.put(flash, &updated)
    }

    pub fn remove<F: NorFlash>(&mut self, flash: &mut F, ieee: u64) -> Result<(), Error> {
        self.table.remove(flash, &ieee.to_le_bytes())
    }

    pub fn compact<F: NorFlash>(&mut self, flash: &mut F) -> Result<(), Error> {
        self.table.compact(flash)
    }

    pub fn bank_state(&self) -> (usize, u32) {
        self.table.bank_state()
    }

    pub fn values(&self) -> impl Iterator<Item = &RecordSlot> {
        self.table.iter().map(|view| view.payload)
    }

    pub fn len(&self) -> usize {
        self.table.len()
    }

    pub fn is_empty(&self) -> bool {
        self.table.is_empty()
    }
}

fn map_record_error(error: RecordError) -> Error {
    match error {
        RecordError::TooLong => Error::InvalidSize,
        RecordError::Truncated
        | RecordError::UnsupportedSchema(_)
        | RecordError::InvalidHeader
        | RecordError::InvalidUtf8 => Error::Corrupt,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::InterviewStatus;
    use micro_db::SimFlash;

    const SIGNATURE: [u8; 7] = [1, 2, 0x06, 0x00, 0x06, 0x00, 0x01];
    const IEEE: u64 = 0xA4C1_383B_F9D6_2A03;

    fn record() -> DeviceRecord<'static> {
        DeviceRecord {
            ieee: IEEE,
            short: 0xD07B,
            interview: InterviewStatus::Complete,
            definition_id: "tuya.ts0041",
            manufacturer: "_TZ3000_b4awzgct",
            model: "TS0041",
            definition_version: 1,
            configured_version: 0,
            signature: &SIGNATURE,
        }
    }

    fn flash() -> SimFlash {
        SimFlash::new(8192, 4096)
    }

    #[test]
    fn record_survives_reboot_and_profile_is_restored() {
        let mut flash = flash();
        let mut registry = Registry::format(&mut flash, 4).unwrap();
        registry.put(&mut flash, &record()).unwrap();

        let restarted = Registry::open(&mut flash, 4).unwrap();
        let profile = restarted.profile_for(IEEE).unwrap().unwrap();
        assert_eq!(profile.id, "tuya.ts0041");
        assert_eq!(restarted.model(IEEE).unwrap().as_deref(), Some("TS0041"));
        let slot = restarted.slot(IEEE).unwrap();
        assert_eq!(slot.record().unwrap().manufacturer, "_TZ3000_b4awzgct");
    }

    #[test]
    fn standard_definition_restores_standard_profile() {
        let mut flash = flash();
        let mut registry = Registry::format(&mut flash, 4).unwrap();
        let mut record = record();
        record.definition_id = crate::standard::STANDARD_PROFILE_ID;
        registry.put(&mut flash, &record).unwrap();

        let restarted = Registry::open(&mut flash, 4).unwrap();
        let profile = restarted.profile_for(IEEE).unwrap().unwrap();
        assert_eq!(profile.id, crate::standard::STANDARD_PROFILE_ID);
    }

    #[test]
    fn configured_version_is_explicit_and_updated() {
        let mut flash = flash();
        let mut registry = Registry::format(&mut flash, 4).unwrap();
        registry.put(&mut flash, &record()).unwrap();
        registry.set_configured(&mut flash, IEEE, 1).unwrap();

        let reopened = Registry::open(&mut flash, 4).unwrap();
        assert_eq!(
            reopened
                .slot(IEEE)
                .unwrap()
                .record()
                .unwrap()
                .configured_version,
            1
        );
    }

    #[test]
    fn configured_flag_is_scoped_to_definition() {
        let mut flash = flash();
        let mut registry = Registry::format(&mut flash, 4).unwrap();
        registry.put(&mut flash, &record()).unwrap();
        registry.set_configured(&mut flash, IEEE, 1).unwrap();
        assert!(registry.is_configured(IEEE, "tuya.ts0041", 1).unwrap());
        assert!(!registry.is_configured(IEEE, "other.profile", 1).unwrap());
        assert!(!registry.is_configured(IEEE, "tuya.ts0041", 2).unwrap());
    }

    #[test]
    fn removal_does_not_return_after_reboot() {
        let mut flash = flash();
        let mut registry = Registry::format(&mut flash, 4).unwrap();
        registry.put(&mut flash, &record()).unwrap();
        registry.remove(&mut flash, IEEE).unwrap();

        let reopened = Registry::open(&mut flash, 4).unwrap();
        assert_eq!(reopened.slot(IEEE), Err(Error::NotFound));
        assert_eq!(reopened.len(), 0);
    }

    #[test]
    fn removal_mark_is_a_durable_tombstone() {
        let mut flash = flash();
        let mut registry = Registry::format(&mut flash, 4).unwrap();
        registry.put(&mut flash, &record()).unwrap();
        registry.mark_removed(&mut flash, IEEE).unwrap();

        let mut reopened = Registry::open(&mut flash, 4).unwrap();
        assert!(reopened.is_removed(IEEE));
        assert!(reopened.profile_for(IEEE).unwrap().is_none());
        assert!(reopened.descriptor_for(IEEE).unwrap().is_none());

        reopened.clear_removed(&mut flash, IEEE).unwrap();
        assert!(!reopened.is_removed(IEEE));
        assert_eq!(reopened.slot(IEEE), Err(Error::NotFound));
    }
}
