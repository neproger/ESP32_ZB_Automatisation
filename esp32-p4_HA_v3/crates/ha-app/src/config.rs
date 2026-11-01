use ha_domain::canonical::{
    ENTITY_AUTOMATION, ENTITY_GROUP, ENTITY_GROUP_AUTOMATION, ENTITY_GROUP_ITEM,
    ENTITY_PROPERTY_STATE, GROUP_ITEM_KEY_LEN, SOURCE_ZIGBEE,
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
    entity: u32,
    key: &[u8],
    record: &[u8],
) -> Result<bool, ha_domain::Error> {
    let changed = domain.transaction::<bool, ha_domain::Error>(|transaction| {
        transaction.entity_put(entity, key, record, meta())
    })?;
    if changed && entity != ENTITY_PROPERTY_STATE {
        crate::store::save_domain(entity, key, record);
    }
    Ok(changed)
}

pub fn remove_record(
    domain: &DomainHandle,
    entity: u32,
    key: &[u8],
) -> Result<bool, ha_domain::Error> {
    let removed = domain.transaction::<bool, ha_domain::Error>(|transaction| match transaction
        .entity_remove(entity, key, meta())
    {
        Ok(()) => Ok(true),
        Err(ha_domain::Error::NotFound) => Ok(false),
        Err(error) => Err(error),
    })?;
    if removed && entity != ENTITY_PROPERTY_STATE {
        crate::store::remove_domain(entity, key);
    }
    Ok(removed)
}

fn remove_record_cascade(
    transaction: &mut DomainTransaction<'_>,
    entity: u32,
    key: &[u8],
    removed: &mut usize,
) -> Result<(), ha_domain::Error> {
    match transaction.entity_remove(entity, key, meta()) {
        Ok(()) => {
            *removed += 1;
            Ok(())
        }
        Err(ha_domain::Error::NotFound) => Ok(()),
        Err(error) => Err(error),
    }
}

pub fn remove_group(domain: &DomainHandle, group: u64) -> Result<usize, ha_domain::Error> {
    domain.transaction::<usize, ha_domain::Error>(|transaction| {
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

        let mut removed = 0;
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

pub fn remove_automation(
    domain: &DomainHandle,
    automation: u64,
) -> Result<usize, ha_domain::Error> {
    domain.transaction::<usize, ha_domain::Error>(|transaction| {
        let id_bytes = automation.to_le_bytes();
        let links = transaction.entity_snapshot(ENTITY_GROUP_AUTOMATION)?;
        let mut link_keys = Vec::new();
        for index in 0..links.len() {
            let key = links.key(index).expect("group automation snapshot key");
            if key.len() == GROUP_ITEM_KEY_LEN && key[8..16] == id_bytes[..] {
                link_keys.push(key.to_vec());
            }
        }

        let mut removed = 0;
        for key in link_keys {
            remove_record_cascade(transaction, ENTITY_GROUP_AUTOMATION, &key, &mut removed)?;
        }
        remove_record_cascade(transaction, ENTITY_AUTOMATION, &id_bytes, &mut removed)?;
        Ok(removed)
    })
}
