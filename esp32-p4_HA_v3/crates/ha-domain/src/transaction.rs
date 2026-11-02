use super::*;

/// Canonical access for one `DomainHandle::transaction` critical section.
///
/// The Domain mutex is held for the whole closure. The transaction serializes
/// reads and writes but does not provide rollback. The closure must stay short
/// and must not perform external calls, async waits, Zigbee/UI/Web work,
/// persistence callbacks, command execution, or nested DomainHandle access.
pub struct DomainTransaction<'a> {
    domain: &'a mut Domain,
}

impl<'a> DomainTransaction<'a> {
    pub(crate) fn new(domain: &'a mut Domain) -> Self {
        Self { domain }
    }

    pub fn entity_snapshot(&mut self, entity: EntityType) -> Result<EntitySnapshot> {
        self.domain.snapshot(entity)
    }

    pub fn entity_meta(&mut self, entity: EntityType, key: &[u8]) -> Result<EntityMeta> {
        self.domain.entity_meta(entity, key)
    }

    pub fn entity_get(&mut self, entity: EntityType, key: &[u8], out: &mut [u8]) -> Result<()> {
        self.domain.entity_get(entity, key, out)
    }

    pub fn entity_put(
        &mut self,
        entity: EntityType,
        key: &[u8],
        record: &[u8],
        meta: FactMeta,
    ) -> Result<bool> {
        self.domain.entity_put(entity, key, record, meta)
    }

    pub fn entity_remove(&mut self, entity: EntityType, key: &[u8], meta: FactMeta) -> Result<()> {
        self.domain.entity_remove(entity, key, meta)
    }

    pub fn entity_record_len(&self, entity: EntityType) -> Result<usize> {
        self.domain.entity_record_len(entity).ok_or(Error::NotFound)
    }
}
