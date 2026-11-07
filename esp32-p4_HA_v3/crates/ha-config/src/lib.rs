#![forbid(unsafe_code)]

use ha_domain::canonical::{
    LocationRecord, SettingsRecord, ENTITY_AUTOMATION, ENTITY_GROUP, ENTITY_GROUP_AUTOMATION,
    ENTITY_GROUP_ITEM, ENTITY_LOCATION, ENTITY_SETTINGS, GROUP_ITEM_KEY_LEN, LOCATION_RECORD_LEN,
    SETTINGS_ID, SETTINGS_RECORD_LEN, SOURCE_SYSTEM, SOURCE_ZIGBEE, SYSTEM_DEVICE_UID,
};
use ha_domain::{DomainHandle, DomainTransaction, FactMeta, Value};

fn meta() -> FactMeta {
    FactMeta {
        source: SOURCE_ZIGBEE as u8,
        value: Value::NONE,
    }
}

pub fn put_record(
    domain: &DomainHandle,
    origin: u8,
    entity: u32,
    key: &[u8],
    record: &[u8],
) -> Result<bool, ha_domain::Error> {
    let meta = FactMeta {
        source: origin,
        value: Value::NONE,
    };
    domain.transaction::<bool, ha_domain::Error>(|transaction| {
        transaction.entity_put(entity, key, record, meta)
    })
}

pub fn seed_defaults(domain: &DomainHandle) {
    let key = SYSTEM_DEVICE_UID.to_le_bytes();
    let mut current = [0u8; LOCATION_RECORD_LEN];
    if domain
        .entity_get(ENTITY_LOCATION, &key, &mut current)
        .is_err()
    {
        let record = LocationRecord::default().encode();
        let meta = FactMeta {
            source: SOURCE_SYSTEM as u8,
            value: Value::NONE,
        };
        match domain.entity_put(ENTITY_LOCATION, &key, &record, meta) {
            Ok(_) => log::info!("domain: location seeded"),
            Err(error) => log::warn!("domain: location seed failed: {error:?}"),
        }
    }

    let key = [SETTINGS_ID];
    let mut current = [0u8; SETTINGS_RECORD_LEN];
    if domain
        .entity_get(ENTITY_SETTINGS, &key, &mut current)
        .is_err()
    {
        let record = SettingsRecord::default().encode();
        match put_record(
            domain,
            SOURCE_SYSTEM as u8,
            ENTITY_SETTINGS,
            &key,
            &record,
        ) {
            Ok(_) => log::info!("domain: settings seeded"),
            Err(error) => log::warn!("domain: settings seed failed: {error:?}"),
        }
    }
}

pub fn remove_record(
    domain: &DomainHandle,
    entity: u32,
    key: &[u8],
) -> Result<bool, ha_domain::Error> {
    domain.transaction::<bool, ha_domain::Error>(|transaction| {
        match transaction.entity_remove(entity, key, meta()) {
            Ok(()) => Ok(true),
            Err(ha_domain::Error::NotFound) => Ok(false),
            Err(error) => Err(error),
        }
    })
}

pub fn remove(
    domain: &DomainHandle,
    entity: u32,
    key: &[u8],
) -> Result<(), ha_domain::Error> {
    let id = || {
        key.get(..8)
            .and_then(|bytes| bytes.try_into().ok())
            .map(u64::from_le_bytes)
            .ok_or(ha_domain::Error::InvalidSize)
    };
    match entity {
        ENTITY_GROUP => id().and_then(|id| remove_group(domain, id)).map(|_| ()),
        ENTITY_AUTOMATION => id()
            .and_then(|id| remove_automation(domain, id))
            .map(|_| ()),
        _ => remove_record(domain, entity, key).map(|_| ()),
    }
}

fn remove_record_cascade(
    transaction: &mut DomainTransaction<'_>,
    entity: u32,
    key: &[u8],
    removed: &mut Vec<(u32, Vec<u8>)>,
) -> Result<(), ha_domain::Error> {
    match transaction.entity_remove(entity, key, meta()) {
        Ok(()) => {
            removed.push((entity, key.to_vec()));
            Ok(())
        }
        Err(ha_domain::Error::NotFound) => Ok(()),
        Err(error) => Err(error),
    }
}

fn remove_group_plan(
    domain: &DomainHandle,
    group: u64,
) -> Result<Vec<(u32, Vec<u8>)>, ha_domain::Error> {
    domain.transaction::<Vec<(u32, Vec<u8>)>, ha_domain::Error>(|transaction| {
        let group_bytes = group.to_le_bytes();
        let group_items = transaction.entity_snapshot(ENTITY_GROUP_ITEM)?;
        let group_automations = transaction.entity_snapshot(ENTITY_GROUP_AUTOMATION)?;
        let mut item_keys = Vec::new();
        let mut automation_keys = Vec::new();
        for index in 0..group_items.len() {
            let key = group_items.key(index).expect("group item snapshot key");
            if key.len() == GROUP_ITEM_KEY_LEN && key[..8] == group_bytes[..] {
                item_keys.push(key.to_vec());
            }
        }
        for index in 0..group_automations.len() {
            let key = group_automations
                .key(index)
                .expect("group automation snapshot key");
            if key.len() == GROUP_ITEM_KEY_LEN && key[..8] == group_bytes[..] {
                automation_keys.push(key.to_vec());
            }
        }

        let mut removed = Vec::new();
        for key in item_keys {
            remove_record_cascade(transaction, ENTITY_GROUP_ITEM, &key, &mut removed)?;
        }
        for key in automation_keys {
            remove_record_cascade(transaction, ENTITY_GROUP_AUTOMATION, &key, &mut removed)?;
        }
        remove_record_cascade(transaction, ENTITY_GROUP, &group_bytes, &mut removed)?;
        Ok(removed)
    })
}

pub fn remove_group(domain: &DomainHandle, group: u64) -> Result<usize, ha_domain::Error> {
    Ok(remove_group_plan(domain, group)?.len())
}

fn remove_automation_plan(
    domain: &DomainHandle,
    automation: u64,
) -> Result<Vec<(u32, Vec<u8>)>, ha_domain::Error> {
    domain.transaction::<Vec<(u32, Vec<u8>)>, ha_domain::Error>(|transaction| {
        let id_bytes = automation.to_le_bytes();
        let links = transaction.entity_snapshot(ENTITY_GROUP_AUTOMATION)?;
        let mut link_keys = Vec::new();
        for index in 0..links.len() {
            let key = links.key(index).expect("group automation snapshot key");
            if key.len() == GROUP_ITEM_KEY_LEN && key[8..16] == id_bytes[..] {
                link_keys.push(key.to_vec());
            }
        }

        let mut removed = Vec::new();
        for key in link_keys {
            remove_record_cascade(transaction, ENTITY_GROUP_AUTOMATION, &key, &mut removed)?;
        }
        remove_record_cascade(transaction, ENTITY_AUTOMATION, &id_bytes, &mut removed)?;
        Ok(removed)
    })
}

pub fn remove_automation(
    domain: &DomainHandle,
    automation: u64,
) -> Result<usize, ha_domain::Error> {
    Ok(remove_automation_plan(domain, automation)?.len())
}

#[cfg(test)]
mod tests {
    use super::*;
    use ha_domain::canonical::{
        AUTOMATION_RECORD_LEN, GROUP_ITEM_RECORD_LEN, GROUP_KEY_LEN, GROUP_RECORD_LEN,
    };
    use ha_domain::{Backing, Domain, EntityDesc};

    fn domain() -> DomainHandle {
        let mut domain = Domain::new(8, 2, 8).unwrap();
        for (entity, key_size, record_size, capacity) in [
            (ENTITY_GROUP, GROUP_KEY_LEN, GROUP_RECORD_LEN, 8),
            (
                ENTITY_GROUP_ITEM,
                GROUP_ITEM_KEY_LEN,
                GROUP_ITEM_RECORD_LEN,
                8,
            ),
            (
                ENTITY_GROUP_AUTOMATION,
                GROUP_ITEM_KEY_LEN,
                GROUP_ITEM_RECORD_LEN,
                8,
            ),
            (ENTITY_AUTOMATION, GROUP_KEY_LEN, AUTOMATION_RECORD_LEN, 8),
        ] {
            domain
                .register_entity(EntityDesc {
                    entity,
                    key_size,
                    record_size,
                    capacity,
                    backing: Backing::Ram,
                    persist_key: None,
                })
                .unwrap();
        }
        DomainHandle::new(domain)
    }

    fn put(domain: &DomainHandle, entity: u32, key: &[u8], len: usize) {
        domain
            .entity_put(entity, key, &vec![0u8; len], meta())
            .unwrap();
    }

    #[test]
    fn group_cascade_lists_group_items_links_and_group() {
        let domain = domain();
        let group = 0x1111_u64;
        let item = 0x2222_u64;
        let automation = 0x3333_u64;
        let mut item_key = [0u8; GROUP_ITEM_KEY_LEN];
        item_key[..8].copy_from_slice(&group.to_le_bytes());
        item_key[8..16].copy_from_slice(&item.to_le_bytes());
        let mut link_key = [0u8; GROUP_ITEM_KEY_LEN];
        link_key[..8].copy_from_slice(&group.to_le_bytes());
        link_key[8..16].copy_from_slice(&automation.to_le_bytes());
        put(
            &domain,
            ENTITY_GROUP,
            &group.to_le_bytes(),
            GROUP_RECORD_LEN,
        );
        put(&domain, ENTITY_GROUP_ITEM, &item_key, GROUP_ITEM_RECORD_LEN);
        put(
            &domain,
            ENTITY_GROUP_AUTOMATION,
            &link_key,
            GROUP_ITEM_RECORD_LEN,
        );

        let removed = remove_group_plan(&domain, group).unwrap();

        assert_eq!(removed.len(), 3);
        assert_eq!(
            domain
                .entity_meta(ENTITY_GROUP, &group.to_le_bytes())
                .unwrap_err(),
            ha_domain::Error::NotFound
        );
        assert!(removed
            .iter()
            .any(|(entity, _)| *entity == ENTITY_GROUP_ITEM));
        assert!(removed
            .iter()
            .any(|(entity, _)| *entity == ENTITY_GROUP_AUTOMATION));
        assert!(removed.iter().any(|(entity, _)| *entity == ENTITY_GROUP));
    }

    #[test]
    fn automation_cascade_lists_links_and_automation() {
        let domain = domain();
        let group = 0x4444_u64;
        let automation = 0x5555_u64;
        let mut link_key = [0u8; GROUP_ITEM_KEY_LEN];
        link_key[..8].copy_from_slice(&group.to_le_bytes());
        link_key[8..16].copy_from_slice(&automation.to_le_bytes());
        put(
            &domain,
            ENTITY_GROUP_AUTOMATION,
            &link_key,
            GROUP_ITEM_RECORD_LEN,
        );
        put(
            &domain,
            ENTITY_AUTOMATION,
            &automation.to_le_bytes(),
            AUTOMATION_RECORD_LEN,
        );

        let removed = remove_automation_plan(&domain, automation).unwrap();

        assert_eq!(removed.len(), 2);
        assert_eq!(
            domain
                .entity_meta(ENTITY_AUTOMATION, &automation.to_le_bytes())
                .unwrap_err(),
            ha_domain::Error::NotFound
        );
    }

    #[test]
    fn missing_records_are_not_included() {
        let domain = domain();
        let removed = remove_group_plan(&domain, 0x6666_u64).unwrap();
        assert!(removed.is_empty());
    }

    #[test]
    fn failed_transaction_returns_no_cleanup_plan() {
        let domain = DomainHandle::new(Domain::new(8, 2, 8).unwrap());
        assert!(remove_group_plan(&domain, 1).is_err());
    }
}
