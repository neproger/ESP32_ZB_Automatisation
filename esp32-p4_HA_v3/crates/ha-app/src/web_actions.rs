use ha_web_service::Actions;

pub struct WebIntake;

impl Actions for WebIntake {
    fn command(
        &self,
        entity: ha_domain::canonical::EntityId,
        property: ha_domain::canonical::PropertyId,
        action: ha_domain::canonical::ActionId,
        value: ha_domain::canonical::CommandValue,
    ) -> Result<(), ha_domain::Error> {
        crate::intake::command(entity, property, action, value)
    }

    fn upsert(&self, entity: u32, key: Vec<u8>, record: Vec<u8>) -> Result<(), ha_domain::Error> {
        crate::intake::upsert(entity, key, record)
    }

    fn remove(&self, entity: u32, key: Vec<u8>) -> Result<(), ha_domain::Error> {
        crate::intake::remove(entity, key)
    }

    fn rename_entity(&self, entity: u64, name: String) -> Result<(), ha_domain::Error> {
        crate::intake::rename_entity(entity, name)
    }

    fn rename_device(&self, device: u64, name: String) -> Result<(), ha_domain::Error> {
        crate::intake::rename_device(device, name)
    }

    fn remove_device(&self, device: u64) -> Result<(), ha_domain::Error> {
        crate::intake::remove_device(device)
    }

    fn open_network(&self, seconds: u8) -> i32 {
        crate::platform::zigbee_open_network(seconds)
    }
}

pub static WEB_ACTIONS: WebIntake = WebIntake;
