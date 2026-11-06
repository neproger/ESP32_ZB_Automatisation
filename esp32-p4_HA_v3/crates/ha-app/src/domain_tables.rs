#![forbid(unsafe_code)]

use ha_domain::canonical::{
    AUTOMATION_RECORD_LEN, ENTITY_AUTOMATION, ENTITY_CAPABILITIES, ENTITY_ENTITY, ENTITY_GROUP,
    ENTITY_GROUP_AUTOMATION, ENTITY_GROUP_ITEM, ENTITY_LOCATION, ENTITY_LOGICAL_DEVICE,
    ENTITY_PROPERTY_STATE, ENTITY_SETTINGS, ENTITY_WEATHER, ENTITY_WIFI_KNOWN, ENTITY_WIFI_SCAN,
    ENTITY_WIFI_STATUS, GROUP_ITEM_KEY_LEN, GROUP_ITEM_RECORD_LEN, GROUP_KEY_LEN, GROUP_RECORD_LEN,
    LOCATION_KEY_LEN, LOCATION_RECORD_LEN, SETTINGS_RECORD_LEN, WEATHER_RECORD_LEN, WIFI_KNOWN_MAX,
    WIFI_KNOWN_RECORD_LEN, WIFI_SCAN_RECORD_LEN, WIFI_STATUS_RECORD_LEN,
};
use ha_domain::{Backing, Domain, EntityDesc};

const STATE_CAPACITY: usize = 256;
const ENTITY_CAPACITY: usize = 32;
const DEVICE_CAPACITY: usize = 32;
const GROUP_CAPACITY: usize = 16;
const GROUP_ITEM_CAPACITY: usize = 128;
const GROUP_AUTOMATION_CAPACITY: usize = 32;
const AUTOMATION_CAPACITY: usize = 32;
const LOCATION_CAPACITY: usize = 4;
const WEATHER_CAPACITY: usize = 1;
const SETTINGS_CAPACITY: usize = 1;

pub fn manifest() -> [EntityDesc<'static>; 14] {
    [
        EntityDesc {
            entity: ENTITY_LOGICAL_DEVICE,
            key_size: 8,
            record_size: ha_domain::canonical::DEVICE_RECORD_LEN,
            capacity: DEVICE_CAPACITY,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_ENTITY,
            key_size: 8,
            record_size: ha_domain::canonical::ENTITY_RECORD_LEN,
            capacity: ENTITY_CAPACITY,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_CAPABILITIES,
            key_size: 8,
            record_size: ha_domain::canonical::CAPABILITIES_LEN,
            capacity: ENTITY_CAPACITY,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_PROPERTY_STATE,
            key_size: ha_domain::canonical::STATE_KEY_LEN,
            record_size: ha_domain::canonical::STATE_RECORD_LEN,
            capacity: STATE_CAPACITY,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_GROUP,
            key_size: GROUP_KEY_LEN,
            record_size: GROUP_RECORD_LEN,
            capacity: GROUP_CAPACITY,
            backing: Backing::RamFlash,
            persist_key: Some("group"),
        },
        EntityDesc {
            entity: ENTITY_GROUP_ITEM,
            key_size: GROUP_ITEM_KEY_LEN,
            record_size: GROUP_ITEM_RECORD_LEN,
            capacity: GROUP_ITEM_CAPACITY,
            backing: Backing::RamFlash,
            persist_key: Some("group_item_v3"),
        },
        EntityDesc {
            entity: ENTITY_GROUP_AUTOMATION,
            key_size: GROUP_ITEM_KEY_LEN,
            record_size: GROUP_ITEM_RECORD_LEN,
            capacity: GROUP_AUTOMATION_CAPACITY,
            backing: Backing::RamFlash,
            persist_key: Some("group_auto_v1"),
        },
        EntityDesc {
            entity: ENTITY_AUTOMATION,
            key_size: GROUP_KEY_LEN,
            record_size: AUTOMATION_RECORD_LEN,
            capacity: AUTOMATION_CAPACITY,
            backing: Backing::RamFlash,
            persist_key: Some("automation_v5"),
        },
        EntityDesc {
            entity: ENTITY_LOCATION,
            key_size: LOCATION_KEY_LEN,
            record_size: LOCATION_RECORD_LEN,
            capacity: LOCATION_CAPACITY,
            backing: Backing::RamFlash,
            persist_key: Some("location"),
        },
        EntityDesc {
            entity: ENTITY_WEATHER,
            key_size: 8,
            record_size: WEATHER_RECORD_LEN,
            capacity: WEATHER_CAPACITY,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_WIFI_SCAN,
            key_size: 8,
            record_size: WIFI_SCAN_RECORD_LEN,
            capacity: 1,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_WIFI_KNOWN,
            key_size: 4,
            record_size: WIFI_KNOWN_RECORD_LEN,
            capacity: WIFI_KNOWN_MAX,
            backing: Backing::RamFlash,
            persist_key: Some("wifi_known"),
        },
        EntityDesc {
            entity: ENTITY_WIFI_STATUS,
            key_size: 8,
            record_size: WIFI_STATUS_RECORD_LEN,
            capacity: 1,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_SETTINGS,
            key_size: 1,
            record_size: SETTINGS_RECORD_LEN,
            capacity: SETTINGS_CAPACITY,
            backing: Backing::RamFlash,
            persist_key: Some("settings"),
        },
    ]
}

pub fn register(domain: &mut Domain) -> Result<(), ha_domain::Error> {
    for desc in manifest() {
        match domain.register_entity(desc) {
            Ok(()) | Err(ha_domain::Error::AlreadyExists) => {}
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use micro_db::{region_size, MAX_ENTRIES};

    const DOMAIN_PARTITION_BYTES: usize = 0x80000;
    const ERASE_SIZE: usize = 4096;
    const DIRECTORY_BYTES: usize = 2 * ERASE_SIZE;
    const COMPACTION_RESERVE: usize = 32 * 1024;

    #[test]
    fn manifest_declares_storage_policy_and_unique_keys() {
        let manifest = manifest();
        let persistent: Vec<_> = manifest
            .iter()
            .filter(|desc| desc.backing == Backing::RamFlash)
            .collect();
        let ram: Vec<_> = manifest
            .iter()
            .filter(|desc| desc.backing == Backing::Ram)
            .collect();

        assert_eq!(persistent.len(), 7);
        assert_eq!(ram.len(), 7);
        assert!(persistent.iter().all(|desc| desc.persist_key.is_some()));
        assert!(ram.iter().all(|desc| desc.persist_key.is_none()));

        let mut keys = std::collections::BTreeSet::new();
        for desc in persistent {
            assert!(keys.insert(desc.persist_key.unwrap()));
        }

        for entity in [ENTITY_LOGICAL_DEVICE, ENTITY_ENTITY, ENTITY_CAPABILITIES] {
            let desc = manifest
                .iter()
                .find(|desc| desc.entity == entity)
                .expect("RAM lifecycle entity");
            assert_eq!(desc.backing, Backing::Ram);
            assert!(desc.persist_key.is_none());
        }

        for (entity, key) in [
            (ENTITY_AUTOMATION, "automation_v5"),
            (ENTITY_GROUP_ITEM, "group_item_v3"),
            (ENTITY_GROUP_AUTOMATION, "group_auto_v1"),
        ] {
            let desc = manifest
                .iter()
                .find(|desc| desc.entity == entity)
                .expect("persistent entity");
            assert_eq!(desc.backing, Backing::RamFlash);
            assert_eq!(desc.persist_key, Some(key));
        }
    }

    #[test]
    fn manifest_matches_canonical_geometry() {
        let manifest = manifest();
        let expected = [
            (
                ENTITY_LOGICAL_DEVICE,
                8,
                ha_domain::canonical::DEVICE_RECORD_LEN,
                DEVICE_CAPACITY,
            ),
            (
                ENTITY_ENTITY,
                8,
                ha_domain::canonical::ENTITY_RECORD_LEN,
                ENTITY_CAPACITY,
            ),
            (
                ENTITY_CAPABILITIES,
                8,
                ha_domain::canonical::CAPABILITIES_LEN,
                ENTITY_CAPACITY,
            ),
            (
                ENTITY_PROPERTY_STATE,
                ha_domain::canonical::STATE_KEY_LEN,
                ha_domain::canonical::STATE_RECORD_LEN,
                STATE_CAPACITY,
            ),
            (
                ENTITY_GROUP,
                GROUP_KEY_LEN,
                GROUP_RECORD_LEN,
                GROUP_CAPACITY,
            ),
            (
                ENTITY_GROUP_ITEM,
                GROUP_ITEM_KEY_LEN,
                GROUP_ITEM_RECORD_LEN,
                GROUP_ITEM_CAPACITY,
            ),
            (
                ENTITY_GROUP_AUTOMATION,
                GROUP_ITEM_KEY_LEN,
                GROUP_ITEM_RECORD_LEN,
                GROUP_AUTOMATION_CAPACITY,
            ),
            (
                ENTITY_AUTOMATION,
                GROUP_KEY_LEN,
                AUTOMATION_RECORD_LEN,
                AUTOMATION_CAPACITY,
            ),
            (
                ENTITY_LOCATION,
                LOCATION_KEY_LEN,
                LOCATION_RECORD_LEN,
                LOCATION_CAPACITY,
            ),
            (ENTITY_WEATHER, 8, WEATHER_RECORD_LEN, WEATHER_CAPACITY),
            (ENTITY_WIFI_SCAN, 8, WIFI_SCAN_RECORD_LEN, 1),
            (ENTITY_WIFI_KNOWN, 4, WIFI_KNOWN_RECORD_LEN, WIFI_KNOWN_MAX),
            (ENTITY_WIFI_STATUS, 8, WIFI_STATUS_RECORD_LEN, 1),
            (ENTITY_SETTINGS, 1, SETTINGS_RECORD_LEN, SETTINGS_CAPACITY),
        ];
        for (entity, key_size, record_size, capacity) in expected {
            let desc = manifest
                .iter()
                .find(|desc| desc.entity == entity)
                .expect("manifest entity");
            assert_eq!(desc.key_size, key_size);
            assert_eq!(desc.record_size, record_size);
            assert_eq!(desc.capacity, capacity);
        }
    }

    #[test]
    fn manifest_fits_domain_partition_with_compaction_reserve() {
        let persistent: Vec<_> = manifest()
            .into_iter()
            .filter(|desc| desc.backing == Backing::RamFlash)
            .collect();
        assert!(persistent.len() <= MAX_ENTRIES);

        let regions = persistent
            .iter()
            .map(|desc| {
                region_size(desc.capacity, desc.key_size, desc.record_size, ERASE_SIZE).unwrap()
            })
            .sum::<usize>();
        let total = regions + DIRECTORY_BYTES + COMPACTION_RESERVE;
        assert!(
            total <= DOMAIN_PARTITION_BYTES,
            "manifest needs {total} bytes, partition is {DOMAIN_PARTITION_BYTES}"
        );
    }
}
