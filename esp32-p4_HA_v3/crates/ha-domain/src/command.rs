use super::*;

pub(crate) fn check_source(meta: &FactMeta) -> Result<()> {
    if meta.source >= 32 {
        return Err(Error::InvalidArg);
    }
    Ok(())
}

pub(crate) struct Command {
    command_type: u32,
    handler: Arc<dyn Fn(&[u8]) -> Result<()> + Send + Sync>,
}

#[derive(Clone)]
struct OwnedTarget {
    entity: EntityType,
    key: Vec<u8>,
}

pub(crate) struct PreparedCommand {
    pub(crate) handler: Arc<dyn Fn(&[u8]) -> Result<()> + Send + Sync>,
    pub(crate) args: Vec<u8>,
    target: Option<OwnedTarget>,
    meta: FactMeta,
    pub(crate) lane: Arc<StdMutex<()>>,
}

impl Domain {
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

    /// Crate-internal direct hand-off for unit tests only; production clients
    /// must use `DomainHandle::post` so the executor runs outside the mutex.
    #[allow(dead_code)]
    pub(crate) fn post(
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
}
