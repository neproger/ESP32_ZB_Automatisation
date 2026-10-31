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

pub use ha_model::canonical;
mod fact;
mod storage;

use std::cell::Cell;
use std::sync::{Arc, Condvar, Mutex as StdMutex, MutexGuard};
use std::thread::LocalKey;
use std::time::{SystemTime, UNIX_EPOCH};

use micro_db::{Geometry, Handle, RamStorage, Ring, Table};

pub use storage::{StorageBackend, StorageProvider};

pub use fact::{
    EntityType, EventId, Fact, FactKind, FactMeta, FactTarget, Filter, Op, PayloadRef, Value,
    ValueType, FACT_KEY_MAX,
};
pub use micro_db::Error;

pub type Result<T> = core::result::Result<T, Error>;

struct DispatchSignal {
    pending: StdMutex<bool>,
    wake: Condvar,
}

impl DispatchSignal {
    fn notify(&self) {
        *self.pending.lock().unwrap() = true;
        self.wake.notify_one();
    }
}

/// Wake handle for the separate Dispatcher task.
#[derive(Clone)]
pub struct DispatchHandle {
    signal: Arc<DispatchSignal>,
}

impl DispatchHandle {
    pub fn wait(&self) {
        let mut pending = self.signal.pending.lock().unwrap();
        while !*pending {
            pending = self.signal.wake.wait(pending).unwrap();
        }
        *pending = false;
    }
}

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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RestorePhase {
    StorageOpen,
    TableOpen,
    Lifecycle,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RestoreError {
    pub entity: EntityType,
    pub phase: RestorePhase,
    pub error: Error,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EntityRestore {
    pub entity: EntityType,
    pub records: usize,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RestoreReport {
    pub entities: Vec<EntityRestore>,
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

thread_local! {
    static IN_DOMAIN_CALLBACK: Cell<bool> = const { Cell::new(false) };
    static IN_COMMAND_EXECUTOR: Cell<bool> = const { Cell::new(false) };
}

struct ThreadFlagGuard(&'static LocalKey<Cell<bool>>);

impl ThreadFlagGuard {
    fn enter(key: &'static LocalKey<Cell<bool>>) -> Self {
        key.with(|flag| flag.set(true));
        Self(key)
    }
}

impl Drop for ThreadFlagGuard {
    fn drop(&mut self) {
        self.0.with(|flag| flag.set(false));
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Lifecycle {
    RamOnly,
    Registering,
    Restored,
}

struct EntityTable {
    desc: EntityDesc<'static>,
    table: Option<Table<StorageBackend>>,
}

enum SubscriptionSink {
    Callback(Box<dyn FnMut(&Fact) -> bool + Send>),
    Channel(std::sync::mpsc::SyncSender<Fact>),
}

struct Subscription {
    filter: Filter,
    sink: SubscriptionSink,
    dropped: u64,
}

struct Command {
    command_type: u32,
    handler: Arc<dyn Fn(&[u8]) -> Result<()> + Send + Sync>,
}

#[derive(Clone)]
struct OwnedTarget {
    entity: EntityType,
    key: Vec<u8>,
}

struct PreparedCommand {
    handler: Arc<dyn Fn(&[u8]) -> Result<()> + Send + Sync>,
    args: Vec<u8>,
    target: Option<OwnedTarget>,
    meta: FactMeta,
    lane: Arc<StdMutex<()>>,
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
    clock: fn() -> u64,
    dispatch: Arc<DispatchSignal>,
    storage: Option<Box<dyn StorageProvider>>,
    lifecycle: Lifecycle,
    command_lane: Arc<StdMutex<()>>,
}

/// Shared Domain boundary. Runtime clients use explicit methods; the hidden
/// `with_mut` remains only for adapter/device mutation until Stage 2.
#[derive(Clone)]
pub struct DomainHandle(Arc<StdMutex<Domain>>);

impl DomainHandle {
    pub fn new(domain: Domain) -> Self {
        Self(Arc::new(StdMutex::new(domain)))
    }

    #[doc(hidden)]
    pub fn with_mut<R>(&self, visit: impl FnOnce(&mut Domain) -> R) -> Result<R> {
        let _flag = ThreadFlagGuard::enter(&IN_DOMAIN_CALLBACK);
        let mut domain = self.lock_checked()?;
        Ok(visit(&mut domain))
    }

    fn lock_checked(&self) -> Result<MutexGuard<'_, Domain>> {
        self.0.lock().map_err(|_| Error::InvalidState)
    }

    pub fn entity_put(
        &self,
        entity: EntityType,
        key: &[u8],
        record: &[u8],
        meta: FactMeta,
    ) -> Result<bool> {
        self.lock_checked()?.entity_put(entity, key, record, meta)
    }

    pub fn entity_get(&self, entity: EntityType, key: &[u8], out: &mut [u8]) -> Result<()> {
        self.lock_checked()?.entity_get(entity, key, out)
    }

    pub fn entity_meta(&self, entity: EntityType, key: &[u8]) -> Result<EntityMeta> {
        self.lock_checked()?.entity_meta(entity, key)
    }

    pub fn entity_remove(&self, entity: EntityType, key: &[u8], meta: FactMeta) -> Result<()> {
        self.lock_checked()?.entity_remove(entity, key, meta)
    }

    pub fn entity_snapshot(&self, entity: EntityType) -> Result<EntitySnapshot> {
        self.lock_checked()?.snapshot(entity)
    }

    pub fn entity_record_len(&self, entity: EntityType) -> Result<usize> {
        self.lock_checked()?
            .entity_record_len(entity)
            .ok_or(Error::NotFound)
    }

    pub fn payload_put(
        &self,
        target: Option<FactTarget<'_>>,
        meta: FactMeta,
        payload: &[u8],
    ) -> Result<PayloadRef> {
        self.lock_checked()?.payload_put(target, meta, payload)
    }

    pub fn payload_get(&self, reference: PayloadRef, out: &mut [u8]) -> Result<()> {
        self.lock_checked()?.payload_get(reference, out)
    }

    pub fn register_command(
        &self,
        command_type: u32,
        handler: impl Fn(&[u8]) -> Result<()> + Send + Sync + 'static,
    ) -> Result<()> {
        self.lock_checked()?.register_command(command_type, handler)
    }

    pub fn post(
        &self,
        command_type: u32,
        args: &[u8],
        target: Option<FactTarget<'_>>,
        meta: FactMeta,
    ) -> Result<()> {
        if IN_DOMAIN_CALLBACK.with(Cell::get) || IN_COMMAND_EXECUTOR.with(Cell::get) {
            return Err(Error::InvalidState);
        }
        let prepared = {
            let mut domain = self.lock_checked()?;
            domain.prepare_post(command_type, args, target, meta)?
        };
        let _executor_flag = ThreadFlagGuard::enter(&IN_COMMAND_EXECUTOR);
        let lane = Arc::clone(&prepared.lane);
        let _lane = lane.lock().map_err(|_| Error::InvalidState)?;
        (prepared.handler)(&prepared.args)?;
        let mut domain = self.lock_checked()?;
        domain.complete_post(&prepared);
        Ok(())
    }

    pub fn dispatch_once(&self) -> Result<usize> {
        Ok(self.lock_checked()?.dispatch_once())
    }

    pub fn dispatch_handle(&self) -> Result<DispatchHandle> {
        Ok(self.lock_checked()?.dispatch_handle())
    }

    pub fn subscriber_drops(&self) -> Result<u64> {
        Ok(self.lock_checked()?.subscriber_drops())
    }

    pub fn gaps(&self) -> Result<u32> {
        Ok(self.lock_checked()?.gaps())
    }

    pub fn dropped_facts(&self) -> Result<u64> {
        Ok(self.lock_checked()?.dropped_facts())
    }
}

impl Domain {
    pub fn new(
        journal_capacity: usize,
        payload_capacity: usize,
        payload_max_size: usize,
    ) -> Result<Self> {
        Self::new_with_clock(journal_capacity, payload_capacity, payload_max_size, now_ms)
    }

    /// The firmware has no wall clock until SNTP: pass the same monotonic clock
    /// the executor uses instead of `SystemTime`.
    pub fn new_with_clock(
        journal_capacity: usize,
        payload_capacity: usize,
        payload_max_size: usize,
        clock: fn() -> u64,
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
            clock,
            dispatch: Arc::new(DispatchSignal {
                pending: StdMutex::new(false),
                wake: Condvar::new(),
            }),
            storage: None,
            lifecycle: Lifecycle::RamOnly,
            command_lane: Arc::new(StdMutex::new(())),
        })
    }

    pub fn new_with_storage(
        journal_capacity: usize,
        payload_capacity: usize,
        payload_max_size: usize,
        clock: fn() -> u64,
        storage: impl StorageProvider + 'static,
    ) -> Result<Self> {
        let mut domain =
            Self::new_with_clock(journal_capacity, payload_capacity, payload_max_size, clock)?;
        domain.storage = Some(Box::new(storage));
        domain.lifecycle = Lifecycle::Registering;
        Ok(domain)
    }

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
            let storage = StorageBackend::new(RamStorage::new(geometry)?);
            Some(Table::open(geometry, storage)?)
        } else {
            None
        };
        self.entities.push(EntityTable { desc, table });
        Ok(())
    }

    pub fn restore(&mut self) -> core::result::Result<RestoreReport, RestoreError> {
        if self.lifecycle != Lifecycle::Registering {
            return Err(RestoreError {
                entity: 0,
                phase: RestorePhase::Lifecycle,
                error: Error::InvalidState,
            });
        }
        let Some(provider) = self.storage.as_mut() else {
            return Err(RestoreError {
                entity: 0,
                phase: RestorePhase::Lifecycle,
                error: Error::InvalidState,
            });
        };

        let mut opened = Vec::with_capacity(self.entities.len());
        let mut report = RestoreReport::default();
        for entity in &self.entities {
            let item = entity.desc.entity;
            let backend = provider.open(&entity.desc).map_err(|error| RestoreError {
                entity: item,
                phase: RestorePhase::StorageOpen,
                error,
            })?;
            let geometry = Geometry {
                capacity: entity.desc.capacity,
                key_size: entity.desc.key_size,
                payload_size: entity.desc.record_size,
            };
            let table = Table::open(geometry, backend).map_err(|error| RestoreError {
                entity: item,
                phase: RestorePhase::TableOpen,
                error,
            })?;
            report.entities.push(EntityRestore {
                entity: item,
                records: table.len(),
            });
            opened.push((item, table));
        }

        for (item, table) in opened {
            if let Some(entity) = self
                .entities
                .iter_mut()
                .find(|entity| entity.desc.entity == item)
            {
                entity.table = Some(table);
            }
        }
        self.storage = None;
        self.lifecycle = Lifecycle::Restored;
        Ok(report)
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

        let changed = self.put_record(entity, key, record)?;
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
        self.table(entity)?.get(key, out)?;
        Ok(())
    }

    pub fn entity_meta(&self, entity: EntityType, key: &[u8]) -> Result<EntityMeta> {
        let (key_size, _) = self.desc(entity)?;
        check_key(key, key_size)?;
        let table = self.table(entity)?;
        let slot = table.find(key)?;
        let meta = table.meta(slot)?;
        Ok(EntityMeta {
            version: meta.version,
        })
    }

    pub fn entity_remove(&mut self, entity: EntityType, key: &[u8], meta: FactMeta) -> Result<()> {
        let (key_size, _) = self.desc(entity)?;
        check_key(key, key_size)?;
        check_source(&meta)?;

        let result = self.remove_record(entity, key);
        if let Err(err) = result {
            if err == Error::NotFound {
                // Separate defect: preserve current NotFound fact behavior in Stage 1c.
                self.publish_error(Op::EntityRemove, meta, entity, key, err);
            }
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

    fn snapshot(&self, entity: EntityType) -> Result<EntitySnapshot> {
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

    /// Borrowed iteration; the callback cannot mutate this domain (borrow checker).
    pub fn entity_iter(
        &self,
        entity: EntityType,
        mut visit: impl FnMut(&[u8], &[u8]),
    ) -> Result<()> {
        let table = self.table(entity)?;
        table.iter(|_, _, key, record| visit(key, record))?;
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
        handler: impl Fn(&[u8]) -> Result<()> + Send + Sync + 'static,
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
            handler: Arc::new(handler),
        });
        Ok(())
    }

    pub(crate) fn prepare_post(
        &mut self,
        command_type: u32,
        args: &[u8],
        target: Option<FactTarget<'_>>,
        meta: FactMeta,
    ) -> Result<PreparedCommand> {
        check_source(&meta)?;
        let target = match target {
            Some(target) => {
                self.desc(target.entity)?;
                Some(OwnedTarget {
                    entity: target.entity,
                    key: target.key.to_vec(),
                })
            }
            None => None,
        };
        let handler = self
            .commands
            .iter()
            .find(|command| command.command_type == command_type)
            .map(|command| Arc::clone(&command.handler))
            .ok_or(Error::NotFound)?;
        Ok(PreparedCommand {
            handler,
            args: args.to_vec(),
            target,
            meta,
            lane: Arc::clone(&self.command_lane),
        })
    }

    pub(crate) fn complete_post(&mut self, prepared: &PreparedCommand) {
        self.publish(
            FactKind::CommandSent,
            Op::Command,
            prepared.meta,
            prepared.target.as_ref().map_or(0, |target| target.entity),
            prepared
                .target
                .as_ref()
                .map_or(&[], |target| target.key.as_slice()),
            prepared.meta.value,
            0,
            0,
            None,
        );
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
        (self.commands[index].handler)(args)?;
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

    pub fn subscribe(&mut self, filter: Filter, sink: impl FnMut(&Fact) -> bool + Send + 'static) {
        self.subscriptions.push(Subscription {
            filter,
            sink: SubscriptionSink::Callback(Box::new(sink)),
            dropped: 0,
        });
    }

    pub fn dispatch_handle(&self) -> DispatchHandle {
        DispatchHandle {
            signal: Arc::clone(&self.dispatch),
        }
    }

    /// V2 subscriber inbox: the dispatcher only performs a non-blocking send.
    /// The receiver belongs to the subscriber task; a full channel is a local
    /// loss and never blocks Domain or the producer.
    pub fn subscribe_channel(&mut self, filter: Filter, sender: std::sync::mpsc::SyncSender<Fact>) {
        self.subscriptions.push(Subscription {
            filter,
            sink: SubscriptionSink::Channel(sender),
            dropped: 0,
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
                if !subscription.filter.matches(&fact) {
                    continue;
                }
                let accepted = match &mut subscription.sink {
                    SubscriptionSink::Callback(callback) => callback(&fact),
                    SubscriptionSink::Channel(sender) => sender.try_send(fact).is_ok(),
                };
                if accepted {
                    delivered += 1;
                } else {
                    subscription.dropped = subscription.dropped.saturating_add(1);
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

    /// Facts rejected by full subscriber inboxes since boot.
    pub fn subscriber_drops(&self) -> u64 {
        self.subscriptions
            .iter()
            .map(|subscription| subscription.dropped)
            .sum()
    }

    fn put_record(&mut self, entity: EntityType, key: &[u8], record: &[u8]) -> Result<bool> {
        let table = self.table_mut(entity)?;
        match table.find(key) {
            Ok(slot) => {
                let meta = table.meta(slot)?;
                table.update(
                    Handle {
                        slot,
                        generation: meta.generation,
                        version: meta.version,
                    },
                    record,
                )
            }
            Err(Error::NotFound) => {
                table.allocate(key, record)?;
                Ok(true)
            }
            Err(err) => Err(err),
        }
    }

    fn remove_record(&mut self, entity: EntityType, key: &[u8]) -> Result<()> {
        let table = self.table_mut(entity)?;
        let slot = table.find(key)?;
        let meta = table.meta(slot)?;
        table.free(Handle {
            slot,
            generation: meta.generation,
            version: meta.version,
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
            (self.clock)(),
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
            Ok(id) => {
                self.dispatch.notify();
                Some(id)
            }
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

    /// Record size for a registered entity type. Copy-out callers need it to
    /// size their buffer; Domain still owns the layout.
    pub fn entity_record_len(&self, entity: EntityType) -> Option<usize> {
        self.desc(entity).ok().map(|(_, record_size)| record_size)
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

    fn table(&self, entity: EntityType) -> Result<&Table<StorageBackend>> {
        self.entity(entity)?
            .table
            .as_ref()
            .ok_or(Error::InvalidState)
    }

    fn table_mut(&mut self, entity: EntityType) -> Result<&mut Table<StorageBackend>> {
        self.entities
            .iter_mut()
            .find(|table| table.desc.entity == entity)
            .and_then(|table| table.table.as_mut())
            .ok_or(Error::InvalidState)
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
    use std::sync::Arc;
    use std::sync::Mutex;

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
    fn injected_clock_sets_fact_time() {
        let mut domain = Domain::new_with_clock(4, 1, 8, || 1_234).unwrap();
        register_device(&mut domain);
        let seen = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&seen);
        domain.subscribe(Filter::all(), move |fact| {
            sink.lock().unwrap().push(fact.ts);
            true
        });
        domain
            .entity_put(DEVICE, &EUI64, &[0u8; 12], meta(Value::NONE))
            .unwrap();
        domain.dispatch_once();
        assert_eq!(*seen.lock().unwrap(), vec![1_234]);
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

        assert_eq!(
            domain.entity_put(DEVICE, &EUI64, &record, meta(Value::NONE)),
            Ok(true)
        );
        assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 1);

        let mut out = [0u8; 12];
        domain.entity_get(DEVICE, &EUI64, &mut out).unwrap();
        assert_eq!(out, record);

        assert_eq!(
            domain.entity_put(DEVICE, &EUI64, &record, meta(Value::NONE)),
            Ok(false)
        );
        assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 1);

        let changed = [2u8; 12];
        assert_eq!(
            domain.entity_put(DEVICE, &EUI64, &changed, meta(Value::NONE)),
            Ok(true)
        );
        assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 2);

        assert_eq!(
            domain.entity_remove(DEVICE, &EUI64, meta(Value::NONE)),
            Ok(())
        );
        assert_eq!(
            domain.entity_get(DEVICE, &EUI64, &mut out),
            Err(Error::NotFound)
        );
        assert_eq!(
            domain.entity_remove(DEVICE, &EUI64, meta(Value::NONE)),
            Err(Error::NotFound)
        );
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
        assert_eq!(
            domain.entity_meta(DEVICE, &EUI64).unwrap_err(),
            Error::NotFound
        );
    }

    #[test]
    fn journal_delivers_only_real_changes_to_matching_subscribers() {
        let mut domain = domain(8);
        register_device(&mut domain);
        domain
            .register_entity(EntityDesc {
                entity: OTHER,
                key_size: 8,
                record_size: 4,
                capacity: 2,
                backing: Backing::Ram,
                persist_key: None,
            })
            .unwrap();

        let seen = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&seen);
        domain.subscribe(
            Filter {
                kind_mask: FactKind::EntityUpserted.bit(),
                ..Filter::entity(DEVICE)
            },
            move |fact| {
                sink.lock()
                    .unwrap()
                    .push((fact.entity, fact.key().to_vec(), fact.value));
                true
            },
        );

        assert!(domain
            .entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::NONE))
            .unwrap());
        assert!(!domain
            .entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::NONE))
            .unwrap());
        assert!(domain
            .entity_put(OTHER, &[7u8; 8], &[0u8; 4], meta(Value::NONE))
            .unwrap());
        assert!(domain
            .entity_put(DEVICE, &EUI64, &[2u8; 12], meta(Value::NONE))
            .unwrap());

        assert_eq!(domain.dispatch_once(), 2);
        let seen = seen.lock().unwrap();
        assert_eq!(seen.len(), 2);
        assert_eq!(seen[0].0, DEVICE);
        assert_eq!(seen[0].1, EUI64);
        assert_eq!(seen[1].1, EUI64);
        assert_eq!(domain.dispatch_once(), 0);
    }

    #[test]
    fn channel_subscription_receives_compact_fact_without_running_subscriber_code() {
        let mut domain = domain(8);
        register_device(&mut domain);
        let (sender, receiver) = std::sync::mpsc::sync_channel(4);
        domain.subscribe_channel(Filter::all(), sender);

        assert!(domain
            .entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::bool(true)))
            .unwrap());
        assert_eq!(domain.dispatch_once(), 1);

        let fact = receiver.try_recv().expect("fact in subscriber inbox");
        assert_eq!(fact.entity, DEVICE);
        assert_eq!(fact.key(), EUI64);
        assert_eq!(fact.value, Value::bool(true));
        assert_eq!(domain.dispatch_once(), 0);
    }

    #[test]
    fn dispatch_handle_wakes_the_dispatcher() {
        let mut domain = domain(8);
        register_device(&mut domain);
        let handle = domain.dispatch_handle();
        let (ready_tx, ready_rx) = std::sync::mpsc::channel();
        let waiter = std::thread::spawn(move || {
            handle.wait();
            ready_tx.send(()).unwrap();
        });
        assert!(domain
            .entity_put(DEVICE, &EUI64, &[1u8; 12], meta(Value::bool(true)))
            .unwrap());
        ready_rx
            .recv_timeout(std::time::Duration::from_secs(1))
            .expect("dispatcher wake");
        waiter.join().unwrap();
    }

    #[test]
    fn full_channel_counts_subscriber_drops() {
        let mut domain = domain(8);
        register_device(&mut domain);
        let (sender, _receiver) = std::sync::mpsc::sync_channel(1);
        domain.subscribe_channel(Filter::all(), sender);

        for on in [true, false, true] {
            assert!(domain
                .entity_put(DEVICE, &EUI64, &[u8::from(on); 12], meta(Value::bool(on)),)
                .unwrap());
            domain.dispatch_once();
        }
        assert_eq!(domain.subscriber_drops(), 2);
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
            domain
                .entity_put(DEVICE, &EUI64, &record, meta(Value::NONE))
                .unwrap();
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

        assert_eq!(
            domain.payload_put(Some(target), meta(Value::u32(1)), b"abc"),
            Ok(1)
        );
        assert_eq!(
            domain.payload_put(Some(target), meta(Value::u32(2)), b"defg"),
            Ok(2)
        );
        let mut buf = [0u8; 8];
        domain.payload_get(2, &mut buf).unwrap();
        assert_eq!(&buf[..4], b"defg");

        assert_eq!(
            domain.payload_put(Some(target), meta(Value::NONE), b"0123456789"),
            Err(Error::InvalidSize)
        );
        assert_eq!(
            domain.payload_put(Some(target), meta(Value::NONE), b"h"),
            Ok(3)
        );
        assert_eq!(domain.payload_get(1, &mut buf), Err(Error::Stale));
        assert_eq!(domain.payload_get(4, &mut buf), Err(Error::NotFound));
        let mut small = [0u8; 0];
        assert_eq!(domain.payload_get(3, &mut small), Err(Error::InvalidSize));

        let events = Arc::new(Mutex::new(Vec::new()));
        let sink = Arc::clone(&events);
        domain.subscribe(Filter::kinds(FactKind::Event.bit()), move |fact| {
            sink.lock()
                .unwrap()
                .push((fact.payload_ref, fact.payload_size, fact.value));
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
        assert_eq!(
            domain.post(7, &[0xaa, 0x01], Some(target), meta(Value::NONE)),
            Ok(())
        );
        assert_eq!(&*args_seen.lock().unwrap(), &[0xaa, 0x01]);
        assert_eq!(
            domain.post(8, &[], Some(target), meta(Value::NONE)),
            Err(Error::NotFound)
        );

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
            deltas
                .iter()
                .map(|value| value.as_enum().unwrap())
                .collect::<Vec<_>>(),
            vec![0, 1, 2]
        );

        let mut record = [0u8; 12];
        domain.entity_get(DEVICE, &EUI64, &mut record).unwrap();
        assert_eq!(record[0], 0x02);
        assert_eq!(domain.entity_meta(DEVICE, &EUI64).unwrap().version, 3);
    }
}
