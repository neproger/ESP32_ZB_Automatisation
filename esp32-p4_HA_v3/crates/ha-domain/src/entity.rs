use super::*;
use crate::command::check_source;
use crate::domain::Lifecycle;
#[cfg(test)]
use std::cell::Cell;

thread_local! {
    #[cfg(test)]
    pub(crate) static FAIL_NEXT_SNAPSHOT: Cell<bool> = const { Cell::new(false) };
}

/// Entity table geometry, declared once at bootstrap.
#[derive(Clone, Copy, Debug)]
pub struct EntityDesc<'a> {
    pub entity: EntityType,
    pub key_size: usize,
    pub record_size: usize,
    pub capacity: usize,
    pub backing: Backing,
    /// Stable persistent identity; required by the flash backend.
    pub persist_key: Option<&'a str>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EntityMeta {
    pub version: u32,
}

pub const ENTITY_SNAPSHOT_MAX_BYTES: usize = 16 * 1024;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EntitySnapshot {
    entity: EntityType,
    key_size: usize,
    record_size: usize,
    count: usize,
    data: Vec<u8>,
}

impl EntitySnapshot {
    pub fn entity(&self) -> EntityType {
        self.entity
    }

    pub fn len(&self) -> usize {
        self.count
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    pub fn key(&self, index: usize) -> Option<&[u8]> {
        if index >= self.count {
            return None;
        }
        let stride = self.key_size + self.record_size;
        let start = index * stride;
        Some(&self.data[start..start + self.key_size])
    }

    pub fn record(&self, index: usize) -> Option<&[u8]> {
        if index >= self.count {
            return None;
        }
        let stride = self.key_size + self.record_size;
        let start = index * stride + self.key_size;
        Some(&self.data[start..start + self.record_size])
    }
}

pub(crate) struct EntityTable {
    pub(crate) desc: EntityDesc<'static>,
    pub(crate) table: Option<Table<SharedStorage>>,
}

pub(crate) fn check_key(key: &[u8], key_size: usize) -> Result<()> {
    if key.len() != key_size {
        return Err(Error::InvalidSize);
    }
    Ok(())
}

fn system_meta() -> FactMeta {
    FactMeta {
        source: crate::canonical::SOURCE_SYSTEM as u8,
        value: Value::NONE,
    }
}

impl Domain {
    pub fn entity_record_len(&self, entity: EntityType) -> Option<usize> {
        self.desc(entity).ok().map(|(_, record_size)| record_size)
    }

    pub(crate) fn desc(&self, entity: EntityType) -> Result<(usize, usize)> {
        let table = self.entity(entity)?;
        Ok((table.desc.key_size, table.desc.record_size))
    }

    pub(crate) fn entity(&self, entity: EntityType) -> Result<&EntityTable> {
        self.entities
            .iter()
            .find(|table| table.desc.entity == entity)
            .ok_or(Error::NotFound)
    }

    pub(crate) fn table(&self, entity: EntityType) -> Result<&Table<SharedStorage>> {
        self.entity(entity)?
            .table
            .as_ref()
            .ok_or(Error::InvalidState)
    }

    pub(crate) fn table_mut(&mut self, entity: EntityType) -> Result<&mut Table<SharedStorage>> {
        self.entities
            .iter_mut()
            .find(|table| table.desc.entity == entity)
            .and_then(|table| table.table.as_mut())
            .ok_or(Error::InvalidState)
    }
}

impl Domain {
    pub fn entity_put(
        &mut self,
        entity: EntityType,
        key: &[u8],
        record: &[u8],
        meta: FactMeta,
    ) -> Result<bool> {
        let (key_size, record_size) = self.desc(entity)?;
        check_key(key, key_size)?;
        if record.len() != record_size {
            return Err(Error::InvalidSize);
        }
        check_source(&meta)?;

        let changed = match self.put_record(entity, key, record) {
            Ok(changed) => changed,
            Err(error) => {
                self.publish_error(Op::EntityPut, meta, entity, key, error);
                return Err(error);
            }
        };
        if changed {
            self.publish(
                FactKind::EntityUpserted,
                Op::EntityPut,
                meta,
                entity,
                key,
                meta.value,
                0,
                0,
                None,
            );
        }
        Ok(changed)
    }

    pub fn entity_get(
        &mut self,
        entity: EntityType,
        key: &[u8],
        out: &mut [u8],
    ) -> Result<()> {
        let (key_size, record_size) = self.desc(entity)?;
        check_key(key, key_size)?;
        if out.len() < record_size {
            return Err(Error::InvalidSize);
        }
        let result = { self.table(entity)?.get(key, out) };
        if let Err(error) = result {
            if error != Error::NotFound {
                self.publish_error(Op::EntityGet, system_meta(), entity, key, error);
            }
            return Err(error);
        }
        Ok(())
    }

    pub fn entity_meta(&mut self, entity: EntityType, key: &[u8]) -> Result<EntityMeta> {
        let (key_size, _) = self.desc(entity)?;
        check_key(key, key_size)?;
        let result = {
            let table = self.table(entity)?;
            table
                .find(key)
                .and_then(|slot| table.meta(slot))
                .map(|meta| EntityMeta {
                    version: meta.version,
                })
        };
        if let Err(error) = result {
            if error != Error::NotFound {
                self.publish_error(Op::EntityGet, system_meta(), entity, key, error);
            }
            return Err(error);
        }
        result
    }

    pub fn entity_remove(&mut self, entity: EntityType, key: &[u8], meta: FactMeta) -> Result<()> {
        let (key_size, _) = self.desc(entity)?;
        check_key(key, key_size)?;
        check_source(&meta)?;

        if let Err(error) = self.remove_record(entity, key) {
            if error != Error::NotFound {
                self.publish_error(Op::EntityRemove, meta, entity, key, error);
            }
            return Err(error);
        }
        self.publish(
            FactKind::EntityRemoved,
            Op::EntityRemove,
            meta,
            entity,
            key,
            meta.value,
            0,
            0,
            None,
        );
        Ok(())
    }

    pub(crate) fn snapshot(&mut self, entity: EntityType) -> Result<EntitySnapshot> {
        let result = self.snapshot_inner(entity);
        if let Err(error) = result {
            if error != Error::NotFound {
                self.publish_error(Op::EntityIter, system_meta(), entity, &[], error);
            }
        }
        result
    }

    fn snapshot_inner(&self, entity: EntityType) -> Result<EntitySnapshot> {
        let table = self.table(entity)?;
        let desc = self.entity(entity)?.desc;
        let count = table.len();
        let stride = desc
            .key_size
            .checked_add(desc.record_size)
            .ok_or(Error::InvalidSize)?;
        let total = count.checked_mul(stride).ok_or(Error::InvalidSize)?;
        if total > ENTITY_SNAPSHOT_MAX_BYTES {
            return Err(Error::InvalidSize);
        }
        // Test-only fault injection at the allocation boundary.
        #[cfg(test)]
        if FAIL_NEXT_SNAPSHOT.with(Cell::get) {
            FAIL_NEXT_SNAPSHOT.with(|flag| flag.set(false));
            return Err(Error::NoMemory);
        }
        let mut data = Vec::new();
        data.try_reserve_exact(total).map_err(|_| Error::NoMemory)?;
        data.resize(total, 0);
        let mut index = 0usize;
        table.iter(|_, _, key, record| {
            let start = index * stride;
            data[start..start + desc.key_size].copy_from_slice(key);
            data[start + desc.key_size..start + stride].copy_from_slice(record);
            index += 1;
        })?;
        if index != count {
            return Err(Error::Corrupt);
        }
        Ok(EntitySnapshot {
            entity,
            key_size: desc.key_size,
            record_size: desc.record_size,
            count,
            data,
        })
    }
}

impl Domain {
    pub fn register_entity(&mut self, desc: EntityDesc<'static>) -> Result<()> {
        if self.lifecycle == Lifecycle::Restored {
            return Err(Error::InvalidState);
        }
        if desc.entity == 0 {
            return Err(Error::InvalidArg);
        }
        if desc.key_size == 0 || desc.key_size > FACT_KEY_MAX {
            return Err(Error::InvalidSize);
        }
        if desc.record_size == 0 || desc.capacity == 0 {
            return Err(Error::InvalidSize);
        }
        if desc.backing == Backing::Flash {
            return Err(Error::InvalidState);
        }
        if let Some(persist_key) = desc.persist_key {
            if self
                .entities
                .iter()
                .any(|table| table.desc.persist_key == Some(persist_key))
            {
                return Err(Error::AlreadyExists);
            }
        }
        if desc.backing == Backing::RamFlash {
            if self.lifecycle == Lifecycle::RamOnly {
                return Err(Error::InvalidState);
            }
            let Some(persist_key) = desc.persist_key else {
                return Err(Error::InvalidArg);
            };
            if persist_key.is_empty() {
                return Err(Error::InvalidArg);
            }
        }
        if self.entity(desc.entity).is_ok() {
            return Err(Error::AlreadyExists);
        }

        let table = if self.lifecycle == Lifecycle::RamOnly {
            let geometry = Geometry {
                capacity: desc.capacity,
                key_size: desc.key_size,
                payload_size: desc.record_size,
            };
            let storage = SharedStorage::new(StorageBackend::new(RamStorage::new(geometry)?));
            Some(Table::open(geometry, storage)?)
        } else {
            None
        };
        self.entities.push(EntityTable { desc, table });
        Ok(())
    }
}
