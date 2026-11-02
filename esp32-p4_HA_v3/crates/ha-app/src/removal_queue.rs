//! Bounded deferred-leave queue for the Zigbee adapter service.

pub(crate) const REMOVAL_QUEUE_CAPACITY: usize = 8;

pub(crate) struct RemovalQueue {
    items: [u64; REMOVAL_QUEUE_CAPACITY],
    len: usize,
}

impl RemovalQueue {
    pub(crate) const fn new() -> Self {
        Self {
            items: [0; REMOVAL_QUEUE_CAPACITY],
            len: 0,
        }
    }

    pub(crate) fn try_push(&mut self, eui: u64) -> Result<(), ha_domain::Error> {
        if self.len == REMOVAL_QUEUE_CAPACITY {
            return Err(ha_domain::Error::NoSpace);
        }
        self.items[self.len] = eui;
        self.len += 1;
        Ok(())
    }

    pub(crate) fn take_all(&mut self) -> ([u64; REMOVAL_QUEUE_CAPACITY], usize) {
        let mut out = [0u64; REMOVAL_QUEUE_CAPACITY];
        out[..self.len].copy_from_slice(&self.items[..self.len]);
        let len = self.len;
        self.len = 0;
        (out, len)
    }
}
