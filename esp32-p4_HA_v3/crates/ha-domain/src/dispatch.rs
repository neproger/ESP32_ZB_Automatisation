use super::*;

pub(crate) struct Subscription {
    pub(crate) filter: Filter,
    pub(crate) sender: std::sync::mpsc::SyncSender<Fact>,
    pub(crate) dropped: u64,
}

pub(crate) struct DispatchSignal {
    pub(crate) pending: StdMutex<bool>,
    pub(crate) wake: Condvar,
}

impl DispatchSignal {
    pub(crate) fn notify(&self) {
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

impl Domain {
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
            sender,
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
                let accepted = subscription.sender.try_send(fact).is_ok();
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
}
