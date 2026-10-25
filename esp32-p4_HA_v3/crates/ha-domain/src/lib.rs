//! Domain core: entity storage, journal of facts, fan-out to subscribers.
//!
//! Semantics follow V2 `domain`: the core stores facts and delivers them, it does
//! not decide what they mean. Entity tables are addressed by an opaque type id and
//! fixed-size key/record bytes; keys and records stay in one binary form shared by
//! storage, UART and WebSocket.
//!
//! * put/remove return `changed` and write the fact only on a real change;
//! * `version` grows only when the record actually changed;
//! * the journal is a bounded ring; the dispatcher starts at the current head and
//!   counts gaps instead of replaying history;
//! * subscribers only copy a fact into their inbox (V2 `try_push` contract).

mod fact;
pub mod canonical;

use std::time::{SystemTime, UNIX_EPOCH};

use micro_db::{Handle, Ring, Table};

pub use fact::{
    EntityType, EventId, Fact, FactKind, FactMeta, FactTarget, Filter, Op, PayloadRef, Value,
    ValueType, FACT_KEY_MAX,
};
pub use micro_db::Error;

pub type Result<T> = core::result::Result<T, Error>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Backing {
    Ram,
    Flash,
    RamFlash,
}

/// Entity table geometry, declared once at bootstrap.
#[derive(Clone, Copy, Debug)]
pub struct EntityDesc<'a> {
    pub entity: EntityType,
    pub key_size: usize,
    pub record_size: usize,
    pub capacity: usize,
    pub backing: Backing,
    /// Stable persistent identity; required by the flash backend (not ported yet).
    pub persist_key: Option<&'a str>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EntityMeta {
    pub version: u32,
}

struct EntityTable {
    desc: EntityDesc<'static>,
    table: Table<Vec<u8>, Vec<u8>>,
}

struct Subscription {
    filter: Filter,
    sink: Box<dyn FnMut(&Fact) -> bool + Send>,
}

struct Command {
    command_type: u32,
    exec: Box<dyn FnMut(&[u8]) -> Result<()> + Send>,
}

pub struct Domain {
    entities: Vec<EntityTable>,
    journal: Ring<Fact>,
    cursor: EventId,
    gaps: u32,
    dropped_facts: u64,
    subscriptions: Vec<Subscription>,
    payloads: Ring<Box<[u8]>>,
    payload_max_size: usize,
    commands: Vec<Command>,
}

impl Domain {
    pub fn new(
        journal_capacity: usize,
        payload_capacity: usize,
        payload_max_size: usize,
    ) -> Result<Self> {
        if payload_max_size == 0 {
            return Err(Error::InvalidSize);
        }
        Ok(Self {
            entities: Vec::new(),
            journal: Ring::new(journal_capacity)?,
            // History is not replayed: start after the current head.
            cursor: 1,
            gaps: 0,
            dropped_facts: 0,
            subscriptions: Vec::new(),
            payloads: Ring::new(payload_capacity)?,
            payload_max_size,
            commands: Vec::new(),
        })
    }

    pub fn register_entity(&mut self, desc: EntityDesc<'static>) -> Result<()> {
        if desc.entity == 0 {
            return Err(Error::InvalidArg);
        }
        if desc.key_size == 0 || desc.key_size > FACT_KEY_MAX {
            return Err(Error::InvalidSize);
        }
        if desc.record_size == 0 || desc.capacity == 0 {
            return Err(Error::InvalidSize);
        }
        if desc.backing != Backing::Ram {
            // Flash persistence is the next step; refusing is better than pretending.
            return Err(Error::InvalidState);
        }
        if self.entity(desc.entity).is_ok() {
            return Err(Error::AlreadyExists);
        }
        self.entities.push(EntityTable {
            desc,
            table: Table::new(desc.capacity)?,
        });
        Ok(())
    }

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

        let result = self.put_record(entity, key, record);
        let changed = match result {
            Ok(changed) => changed,
            Err(err) => {
                self.publish_error(Op::EntityPut, meta, entity, key, err);
                return Err(err);
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

    pub fn entity_get(&self, entity: EntityType, key: &[u8], out: &mut [u8]) -> Result<()> {
        let (key_size, record_size) = self.desc(entity)?;
        check_key(key, key_size)?;
        if out.len() < record_size {
            return Err(Error::InvalidSize);
        }
        let table = &self.entity(entity)?.table;
        let slot = table.find(key)?;
        let view = table.read(slot)?;
        out[..record_size].copy_from_slice(view.payload);
        Ok(())
    }

    pub fn entity_meta(&self, entity: EntityType, key: &[u8]) -> Result<EntityMeta> {
        let (key_size, _) = self.desc(entity)?;
        check_key(key, key_size)?;
        let table = &self.entity(entity)?.table;
        let slot = table.find(key)?;
        let meta = table.meta(slot)?;
        Ok(EntityMeta {
            version: meta.version,
        })
    }

    pub fn entity_remove(
        &mut self,
        entity: EntityType,
        key: &[u8],
        meta: FactMeta,
    ) -> Result<()> {
        let (key_size, _) = self.desc(entity)?;
        check_key(key, key_size)?;
        check_source(&meta)?;

        let result = self.remove_record(entity, key);
        if let Err(err) = result {
            self.publish_error(Op::EntityRemove, meta, entity, key, err);
            return Err(err);
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

    /// Borrowed iteration; the callback cannot mutate this domain (borrow checker).
    pub fn entity_iter(
        &self,
        entity: EntityType,
        mut visit: impl FnMut(&[u8], &[u8]),
    ) -> Result<()> {
        let table = &self.entity(entity)?.table;
        for view in table.iter() {
            visit(view.key, view.payload);
        }
        Ok(())
    }

    pub fn payload_put(
        &mut self,
        target: Option<FactTarget<'_>>,
        meta: FactMeta,
        payload: &[u8],
    ) -> Result<PayloadRef> {
        if payload.is_empty() || payload.len() > self.payload_max_size {
            return Err(Error::InvalidSize);
        }
        check_source(&meta)?;
        if let Some(target) = target {
            self.desc(target.entity)?;
        }
        let reference = self.payloads.append(payload.into())?;
        self.publish(
            FactKind::Event,
            Op::PayloadPut,
            meta,
            target.map_or(0, |target| target.entity),
            target.map_or(&[], |target| target.key),
            meta.value,
            reference,
            payload.len() as u32,
            None,
        );
        Ok(reference)
    }

    pub fn payload_get(&self, reference: PayloadRef, out: &mut [u8]) -> Result<()> {
        let bytes = self.payloads.get_ref(reference)?;
        if out.len() < bytes.len() {
            return Err(Error::InvalidSize);
        }
        out[..bytes.len()].copy_from_slice(bytes);
        Ok(())
    }

    pub fn register_command(
        &mut self,
        command_type: u32,
        exec: impl FnMut(&[u8]) -> Result<()> + Send + 'static,
    ) -> Result<()> {
        if self
            .commands
            .iter()
            .any(|command| command.command_type == command_type)
        {
            return Err(Error::AlreadyExists);
        }
        self.commands.push(Command {
            command_type,
            exec: Box::new(exec),
        });
        Ok(())
    }

    /// Synchronous hand-off; the executor's result is not awaited.
    pub fn post(
        &mut self,
        command_type: u32,
        args: &[u8],
        target: Option<FactTarget<'_>>,
        meta: FactMeta,
    ) -> Result<()> {
        check_source(&meta)?;
        if let Some(target) = target {
            self.desc(target.entity)?;
        }
        let index = self
            .commands
            .iter()
            .position(|command| command.command_type == command_type)
            .ok_or(Error::NotFound)?;
        (self.commands[index].exec)(args)?;
        self.publish(
            FactKind::CommandSent,
            Op::Command,
            meta,
            target.map_or(0, |target| target.entity),
            target.map_or(&[], |target| target.key),
            meta.value,
            0,
            0,
            None,
        );
        Ok(())
    }

    pub fn subscribe(
        &mut self,
        filter: Filter,
        sink: impl FnMut(&Fact) -> bool + Send + 'static,
    ) {
        self.subscriptions.push(Subscription {
            filter,
            sink: Box::new(sink),
        });
    }

    /// Delivers facts written since the last call. Returns the number of
    /// subscriber hand-offs accepted by their inboxes.
    pub fn dispatch_once(&mut self) -> usize {
        let newest = match self.journal.newest_seq() {
            Ok(newest) => newest,
            Err(_) => return 0,
        };
        let oldest = self
            .journal
            .oldest_seq()
            .expect("non-empty journal has oldest");
        if self.cursor < oldest {
            // Facts were evicted before delivery; record it instead of hiding it.
            self.gaps += 1;
            self.cursor = oldest;
        }
        let mut delivered = 0;
        while self.cursor <= newest {
            let fact = match self.journal.get(self.cursor) {
                Ok(fact) => fact,
                Err(_) => break,
            };
            for subscription in &mut self.subscriptions {
                if subscription.filter.matches(&fact) && (subscription.sink)(&fact) {
                    delivered += 1;
                }
            }
            self.cursor += 1;
        }
        delivered
    }

    /// Journal gaps since boot (events evicted before dispatch).
    pub fn gaps(&self) -> u32 {
        self.gaps
    }

    /// Facts lost because the journal ring rejected an append (u64 overflow only).
    pub fn dropped_facts(&self) -> u64 {
        self.dropped_facts
    }

    fn put_record(&mut self, entity: EntityType, key: &[u8], record: &[u8]) -> Result<bool> {
        let table = &mut self.table_mut(entity)?.table;
        match table.find(key) {
            Ok(slot) => {
                let meta = table.meta(slot)?;
                table.update(
                    Handle {
                        slot,
                        generation: meta.generation,
                    },
                    record.to_vec(),
                )
            }
            Err(Error::NotFound) => {
                table.allocate(key.to_vec(), record.to_vec())?;
                Ok(true)
            }
            Err(err) => Err(err),
        }
    }

    fn remove_record(&mut self, entity: EntityType, key: &[u8]) -> Result<()> {
        let table = &mut self.table_mut(entity)?.table;
        let slot = table.find(key)?;
        let meta = table.meta(slot)?;
        table.free(Handle {
            slot,
            generation: meta.generation,
        })
    }

    #[allow(clippy::too_many_arguments)]
    fn publish(
        &mut self,
        kind: FactKind,
        op: Op,
        meta: FactMeta,
        entity: EntityType,
        key: &[u8],
        value: Value,
        payload_ref: PayloadRef,
        payload_size: u32,
        error: Option<Error>,
    ) -> Option<EventId> {
        let event_id = self.journal.next_seq();
        let fact = Fact::new(
            event_id,
            now_ms(),
            kind,
            op,
            meta.source,
            entity,
            key,
            value,
            payload_ref,
            payload_size,
            error,
        );
        match self.journal.append(fact) {
            Ok(id) => Some(id),
            Err(_) => {
                // The journal is a parallel trace: state stays committed, but the
                // loss is counted instead of vanishing.
                self.dropped_facts += 1;
                None
            }
        }
    }

    fn publish_error(
        &mut self,
        op: Op,
        meta: FactMeta,
        entity: EntityType,
        key: &[u8],
        error: Error,
    ) {
        self.publish(
            FactKind::Error,
            op,
            meta,
            entity,
            key,
            meta.value,
            0,
            0,
            Some(error),
        );
    }

    fn desc(&self, entity: EntityType) -> Result<(usize, usize)> {
        let table = self.entity(entity)?;
        Ok((table.desc.key_size, table.desc.record_size))
    }

    fn entity(&self, entity: EntityType) -> Result<&EntityTable> {
        self.entities
            .iter()
            .find(|table| table.desc.entity == entity)
            .ok_or(Error::NotFound)
    }

    fn table_mut(&mut self, entity: EntityType) -> Result<&mut EntityTable> {
        self.entities
            .iter_mut()
            .find(|table| table.desc.entity == entity)
            .ok_or(Error::NotFound)
    }
}

fn check_key(key: &[u8], key_size: usize) -> Result<()> {
    if key.len() != key_size {
        return Err(Error::InvalidSize);
    }
    Ok(())
}

fn check_source(meta: &FactMeta) -> Result<()> {
    if meta.source >= 32 {
        return Err(Error::InvalidArg);
    }
    Ok(())
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_millis() as u64)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;
    use std::sync::Arc;

    const DEVICE: EntityType = 1;
    const OTHER: EntityType = 2;
    const SOURCE_ZIGBEE: u8 = 1;
    const EUI64: [u8; 8] = [0xa4, 0xc1, 0x38, 0x3b, 0xf9, 0xd6, 0x2a, 0x03];

    fn domain(journal: usize) -> Domain {
        Domain::new(journal, 4, 16).unwrap()
    }

    fn register_device(domain: &mut Domain) {
        domain
            .register_entity(EntityDesc {
                entity: DEVICE,
                key_size: 8,
                record_size: 12,
                capacity: 4,
                backing: Backing::Ram,
                persist_key: None,
            })
            .unwrap();
    }

    fn meta(value: Value) -> FactMeta {
        FactMeta {
            source: SOURCE_ZIGBEE,
            value,
        }
    }

    #[test]
    fn registration_validates_geometry_and_duplicates() {
        let mut domain = domain(4);
        register_device(&mut domain);
        assert_eq!(
            domain.register_entity(EntityDesc {
                entity: DEVICE,
                key_size: 8,
                record_size: 12,
                capacity: 4,
                backing: Backing::Ram,
                persist_key: None,
            }),
            Err(Error::AlreadyExists)
        );
        assert_eq!(
            domain.register_entity(EntityDesc {
                entity: 3,
                key_size: FACT_KEY_MAX + 1,
                record_size: 1,
                capacity: 1,
                backing: Backing::Ram,
                persist_key: None,
            }),
            Err(Error::InvalidSize)
        );
        assert_eq!(
            domain.register_entity(EntityDesc {
                entity: 4,
                key_size: 8,
                record_size: 12,
                capacity: 4,
                backing: Backing::Flash,
                persist_key: Some("devices"),
            }),
            Err(Error::InvalidState)
        );
    }

    #[test]
    fn put_get_meta_remove() {
        let mut domain = domain(8);
        register_device(&mut domain);
        let record = [1u8; 12];

        assert_eq!(domain.entity_put(DEVICE, &EUI64, &record, meta(Value::NONE)), Ok(true));
        assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 1);

        let mut out = [0u8; 12];
        domain.entity_get(DEVICE, &EUI64, &mut out).unwrap();
        assert_eq!(out, record);

        assert_eq!(domain.entity_put(DEVICE, &EUI64, &record, meta(Value::NONE)), Ok(false));
        assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 1);

        let changed = [2u8; 12];
        assert_eq!(domain.entity_put(DEVICE, &EUI64, &changed, meta(Value::NONE)), Ok(true));
        assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 2);

        assert_eq!(domain.entity_remove(DEVICE, &EUI64, meta(Value::NONE)), Ok(()));
        assert_eq!(domain.entity_get(DEVICE, &EUI64, &mut out), Err(Error::NotFound));
        assert_eq!(domain.entity_remove(DEVICE, &EUI64, meta(Value::NONE)), Err(Error::NotFound));
    }

    #[test]
    fn size_and_source_are_checked_before_mutation() {
        let mut domain = domain(4);
        register_device(&mut domain);
        assert_eq!(
            domain.entity_put(DEVICE, &[0u8; 4], &[0u8; 12], meta(Value::NONE)),
            Err(Error::InvalidSize)
        );
        assert_eq!(
            domain.entity_put(DEVICE, &EUI64, &[0u8; 11], meta(Value::NONE)),
            Err(Error::InvalidSize)
        );
        assert_eq!(
            domain.entity_put(
                DEVICE,
                &EUI64,
                &[0u8; 12],
                FactMeta {
                    source: 32,
                    value: Value::NONE
                }
            ),
            Err(Error::InvalidArg)
        );
        assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap_err(), Error::NotFound);
    }

    #[test]
    fn journal_delivers_only_real_changes_to_matching_subscribers() {
        let mut domain = domain(8);
        register_device(&mut domain);
        domain.register_entity(EntityDesc {
            entity: OTHER,
            key_size: 8,
            record_size: 4,
            capacity: 2,
            backing: Backing::Ram,
            persist_key: None,
        }).unwrap();

        let seen = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&seen);
        domain.subscribe(
            Filter {
                kind_mask: FactKind::EntityUpserted.bit(),
                ..Filter::entity(DEVICE)
            },
            move |fact| {
                sink.lock().unwrap().push((fact.entity, fact.key().to_vec(), fact.value));
                true
            },
        );

        assert!(domain.entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::NONE)).unwrap());
        assert!(!domain.entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::NONE)).unwrap());
        assert!(domain.entity_put(OTHER, &[7u8; 8], &[0u8; 4], meta(Value::NONE)).unwrap());
        assert!(domain.entity_put(DEVICE, &EUI64, &[2u8; 12], meta(Value::NONE)).unwrap());

        assert_eq!(domain.dispatch_once(), 2);
        let seen = seen.lock().unwrap();
        assert_eq!(seen.len(), 2);
        assert_eq!(seen[0].0, DEVICE);
        assert_eq!(seen[0].1, EUI64);
        assert_eq!(seen[1].1, EUI64);
        assert_eq!(domain.dispatch_once(), 0);
    }

    #[test]
    fn dispatcher_reports_evicted_facts_as_a_gap() {
        let mut domain = domain(2);
        register_device(&mut domain);
        let seen = Arc::new(Mutex::new(0u32));
        let sink = Arc::clone(&seen);
        domain.subscribe(Filter::all(), move |_| {
            *sink.lock().unwrap() += 1;
            true
        });

        for value in [1u8, 2, 3] {
            let mut record = [0u8; 12];
            record[0] = value;
            domain.entity_put(DEVICE, &EUI64, &record, meta(Value::NONE)).unwrap();
        }
        assert_eq!(domain.dispatch_once(), 2);
        assert_eq!(*seen.lock().unwrap(), 2);
        assert_eq!(domain.gaps(), 1);
    }

    #[test]
    fn payload_ring_publishes_and_evicts() {
        let mut domain = Domain::new(4, 2, 8).unwrap();
        register_device(&mut domain);
        let target = FactTarget {
            entity: DEVICE,
            key: &EUI64,
        };

        assert_eq!(domain.payload_put(Some(target), meta(Value::u32(1)), b"abc"), Ok(1));
        assert_eq!(domain.payload_put(Some(target), meta(Value::u32(2)), b"defg"), Ok(2));
        let mut buf = [0u8; 8];
        domain.payload_get(2, &mut buf).unwrap();
        assert_eq!(&buf[..4], b"defg");

        assert_eq!(domain.payload_put(Some(target), meta(Value::NONE), b"0123456789"), Err(Error::InvalidSize));
        assert_eq!(domain.payload_put(Some(target), meta(Value::NONE), b"h"), Ok(3));
        assert_eq!(domain.payload_get(1, &mut buf), Err(Error::Stale));
        assert_eq!(domain.payload_get(4, &mut buf), Err(Error::NotFound));
        let mut small = [0u8; 0];
        assert_eq!(domain.payload_get(3, &mut small), Err(Error::InvalidSize));

        let events = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&events);
        domain.subscribe(Filter::kinds(FactKind::Event.bit()), move |fact| {
            sink.lock().unwrap().push((fact.payload_ref, fact.payload_size, fact.value));
            true
        });
        assert_eq!(domain.dispatch_once(), 3);
        let events = events.lock().unwrap();
        assert_eq!(events[1], (2, 4, Value::u32(2)));
        assert_eq!(events[2].0, 3);
    }

    #[test]
    fn commands_are_handed_off_and_journaled() {
        let mut domain = domain(4);
        register_device(&mut domain);
        let args_seen = Arc::new(Mutex::new(Vec::new()));
        let recorded = Arc::clone(&args_seen);
        domain
            .register_command(7, move |args| {
                recorded.lock().unwrap().extend_from_slice(args);
                Ok(())
            })
            .unwrap();

        let target = FactTarget {
            entity: DEVICE,
            key: &EUI64,
        };
        assert_eq!(domain.post(7, &[0xaa, 0x01], Some(target), meta(Value::NONE)), Ok(()));
        assert_eq!(&*args_seen.lock().unwrap(), &[0xaa, 0x01]);
        assert_eq!(domain.post(8, &[], Some(target), meta(Value::NONE)), Err(Error::NotFound));

        let seen = Arc::new(Mutex::new(0u32));
        let sink = Arc::clone(&seen);
        domain.subscribe(Filter::kinds(FactKind::CommandSent.bit()), move |fact| {
            assert_eq!(fact.op, Op::Command);
            assert_eq!(fact.key(), EUI64);
            *sink.lock().unwrap() += 1;
            true
        });
        assert_eq!(domain.dispatch_once(), 1);
        assert_eq!(*seen.lock().unwrap(), 1);
    }

    /// Vertical slice: the exact event captured from TS0041 on hardware
    /// (OnOff 0xFD payload 00/01/02 = single/double/hold) reaches the store and
    /// produces one delta per real change.
    #[test]
    fn ts0041_event_becomes_entity_state_and_delta() {
        let mut domain = domain(8);
        register_device(&mut domain);
        let deltas = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&deltas);
        domain.subscribe(
            Filter {
                kind_mask: FactKind::EntityUpserted.bit(),
                ..Filter::entity(DEVICE)
            },
            move |fact| {
                assert_eq!(fact.source, SOURCE_ZIGBEE);
                assert_eq!(fact.key(), EUI64);
                assert_eq!(fact.op, Op::EntityPut);
                sink.lock().unwrap().push(fact.value);
                true
            },
        );

        let events = [(0x00u8, 0u32), (0x01, 1), (0x02, 2)];
        for (payload, press) in events {
            let mut record = [0u8; 12];
            record[0] = payload;
            let changed = domain
                .entity_put(DEVICE, &EUI64, &record, meta(Value::enum_(press)))
                .unwrap();
            assert!(changed);
        }

        assert_eq!(domain.dispatch_once(), 3);
        let deltas = deltas.lock().unwrap();
        assert_eq!(
            deltas.iter().map(|value| value.as_enum().unwrap()).collect::<Vec<_>>(),
            vec![0, 1, 2]
        );

        let mut record = [0u8; 12];
        domain.entity_get(DEVICE, &EUI64, &mut record).unwrap();
        assert_eq!(record[0], 0x02);
        assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 3);
    }
}
