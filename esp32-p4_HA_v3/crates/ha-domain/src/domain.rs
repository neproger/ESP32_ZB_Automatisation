use super::*;

use crate::entity::EntityTable;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Lifecycle {
    RamOnly,
    Registering,
    Restored,
}

pub struct Domain {
    pub(crate) entities: Vec<EntityTable>,
    pub(crate) journal: Ring<Fact>,
    pub(crate) cursor: EventId,
    pub(crate) gaps: u32,
    pub(crate) dropped_facts: u64,
    pub(crate) subscriptions: Vec<Subscription>,
    pub(crate) payloads: Ring<Box<[u8]>>,
    pub(crate) payload_max_size: usize,
    pub(crate) commands: Vec<Command>,
    pub(crate) clock: fn() -> u64,
    pub(crate) dispatch: Arc<DispatchSignal>,
    pub(crate) storage: Option<Box<dyn StorageProvider>>,
    pub(crate) maintenance: Option<DomainMaintenance>,
    pub(crate) lifecycle: Lifecycle,
    pub(crate) command_lane: Arc<StdMutex<()>>,
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
            maintenance: None,
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
        let mut maintenance = Vec::with_capacity(self.entities.len());
        let mut report = RestoreReport::default();
        for entity in &self.entities {
            let item = entity.desc.entity;
            let backend = provider.open(&entity.desc).map_err(|error| RestoreError {
                entity: item,
                phase: RestorePhase::StorageOpen,
                error,
            })?;
            let shared = SharedStorage::new(backend);
            let geometry = Geometry {
                capacity: entity.desc.capacity,
                key_size: entity.desc.key_size,
                payload_size: entity.desc.record_size,
            };
            let table = Table::open(geometry, shared.clone()).map_err(|error| RestoreError {
                entity: item,
                phase: RestorePhase::TableOpen,
                error,
            })?;
            report.entities.push(EntityRestore {
                entity: item,
                records: table.len(),
            });
            opened.push((item, table));
            maintenance.push((item, shared));
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
        self.maintenance = Some(DomainMaintenance::new(maintenance));
        self.lifecycle = Lifecycle::Restored;
        Ok(report)
    }

    pub fn take_maintenance(&mut self) -> Option<DomainMaintenance> {
        self.maintenance.take()
    }
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_millis() as u64)
}
