use super::*;
use std::cell::Cell;
use std::thread::LocalKey;

thread_local! {
    static IN_DOMAIN_CALLBACK: Cell<bool> = const { Cell::new(false) };
    static IN_COMMAND_EXECUTOR: Cell<bool> = const { Cell::new(false) };
    static IN_TRANSACTION: Cell<bool> = const { Cell::new(false) };
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

#[derive(Clone)]
pub struct DomainHandle(Arc<StdMutex<Domain>>);

impl DomainHandle {
    pub fn new(domain: Domain) -> Self {
        Self(Arc::new(StdMutex::new(domain)))
    }

    /// Test-only transitional escape hatch. Production code must use
    /// `transaction` or a typed Domain method; no production call sites remain.
    #[doc(hidden)]
    pub fn with_mut<R>(&self, visit: impl FnOnce(&mut Domain) -> R) -> Result<R> {
        let _flag = ThreadFlagGuard::enter(&IN_DOMAIN_CALLBACK);
        let mut domain = self.lock_checked()?;
        Ok(visit(&mut domain))
    }

    /// Runs a short critical section under the Domain mutex.
    ///
    /// The closure may use only [`DomainTransaction`] operations. It must not
    /// call external services, wait for async work, use persistence callbacks,
    /// run the command executor, or call back into `DomainHandle`. The mutex is
    /// held for the whole closure; changes are serialized but never rolled back.
    /// A panic can poison the Domain mutex.
    pub fn transaction<R, E>(
        &self,
        visit: impl FnOnce(&mut DomainTransaction<'_>) -> core::result::Result<R, E>,
    ) -> core::result::Result<R, E>
    where
        E: From<Error>,
    {
        if IN_TRANSACTION.with(Cell::get)
            || IN_DOMAIN_CALLBACK.with(Cell::get)
            || IN_COMMAND_EXECUTOR.with(Cell::get)
        {
            return Err(E::from(Error::InvalidState));
        }
        let mut domain = self.lock_transaction().map_err(E::from)?;
        let _flag = ThreadFlagGuard::enter(&IN_TRANSACTION);
        visit(&mut DomainTransaction::new(&mut domain))
    }

    #[cfg(not(feature = "p4-lock-metrics"))]
    fn lock_transaction(&self) -> Result<DomainGuard<'_>> {
        self.0.lock().map_err(|_| Error::InvalidState)
    }

    #[cfg(feature = "p4-lock-metrics")]
    fn lock_transaction(&self) -> Result<DomainGuard<'_>> {
        let started = Instant::now();
        let guard = self.0.lock().map_err(|_| Error::InvalidState)?;
        record_wait(elapsed_us(started));
        Ok(MeasuredGuard {
            guard,
            holding_since: Instant::now(),
        })
    }

    fn lock_checked(&self) -> Result<DomainGuard<'_>> {
        if IN_TRANSACTION.with(Cell::get) {
            return Err(Error::InvalidState);
        }
        self.lock_transaction()
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
        if IN_TRANSACTION.with(Cell::get)
            || IN_DOMAIN_CALLBACK.with(Cell::get)
            || IN_COMMAND_EXECUTOR.with(Cell::get)
        {
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
