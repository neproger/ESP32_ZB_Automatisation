use super::*;
use crate::command::check_source;

impl Domain {
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

    pub(crate) fn put_record(
        &mut self,
        entity: EntityType,
        key: &[u8],
        record: &[u8],
    ) -> Result<bool> {
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

    pub(crate) fn remove_record(&mut self, entity: EntityType, key: &[u8]) -> Result<()> {
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
    pub(crate) fn publish(
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

    pub(crate) fn publish_error(
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
}
