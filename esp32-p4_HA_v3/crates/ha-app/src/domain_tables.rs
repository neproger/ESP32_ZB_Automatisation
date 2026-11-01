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

pub fn register(domain: &mut Domain) -> Result<(), ha_domain::Error> {
    let tables = [
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
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_GROUP_ITEM,
            key_size: GROUP_ITEM_KEY_LEN,
            record_size: GROUP_ITEM_RECORD_LEN,
            capacity: GROUP_ITEM_CAPACITY,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_GROUP_AUTOMATION,
            key_size: GROUP_ITEM_KEY_LEN,
            record_size: GROUP_ITEM_RECORD_LEN,
            capacity: GROUP_AUTOMATION_CAPACITY,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_AUTOMATION,
            key_size: GROUP_KEY_LEN,
            record_size: AUTOMATION_RECORD_LEN,
            capacity: AUTOMATION_CAPACITY,
            backing: Backing::Ram,
            persist_key: None,
        },
        EntityDesc {
            entity: ENTITY_LOCATION,
            key_size: LOCATION_KEY_LEN,
            record_size: LOCATION_RECORD_LEN,
            capacity: LOCATION_CAPACITY,
            backing: Backing::Ram,
            persist_key: None,
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
            backing: Backing::Ram,
            persist_key: None,
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
            backing: Backing::Ram,
            persist_key: None,
        },
    ];
    for desc in tables {
        match domain.register_entity(desc) {
            Ok(()) | Err(ha_domain::Error::AlreadyExists) => {}
            Err(error) => return Err(error),
        }
    }
    Ok(())
}
