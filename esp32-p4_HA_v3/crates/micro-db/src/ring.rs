use crate::table::{Error, Result};

/// Bounded window of sequential records; the oldest is overwritten once full.
///
/// `seq` is the durable identity of a record (`1..`). It is monotonic and never
/// reused, so an evicted `seq` is distinguishable from one that never existed:
/// the former is `Stale`, the latter `NotFound`.
pub struct Ring<T> {
    records: Vec<Option<T>>,
    next_seq: u64,
    count: usize,
}

impl<T: Clone> Ring<T> {
    pub fn new(capacity: usize) -> Result<Self> {
        if capacity == 0 {
            return Err(Error::InvalidSize);
        }
        Ok(Self {
            records: vec![None; capacity],
            next_seq: 1,
            count: 0,
        })
    }

    pub fn append(&mut self, record: T) -> Result<u64> {
        if self.next_seq == u64::MAX {
            return Err(Error::Overflow);
        }
        let seq = self.next_seq;
        let slot = ((seq - 1) % self.records.len() as u64) as usize;
        self.records[slot] = Some(record);
        self.next_seq += 1;
        if self.count < self.records.len() {
            self.count += 1;
        }
        Ok(seq)
    }

    pub fn get(&self, seq: u64) -> Result<T> {
        self.get_ref(seq).cloned()
    }

    /// Borrow the record inside the window without copying.
    pub fn get_ref(&self, seq: u64) -> Result<&T> {
        if seq == 0 || seq >= self.next_seq {
            return Err(Error::NotFound);
        }
        if seq < self.oldest_seq()? {
            return Err(Error::Stale);
        }
        let slot = ((seq - 1) % self.records.len() as u64) as usize;
        self.records[slot].as_ref().ok_or(Error::InvariantFailed)
    }

    pub fn oldest_seq(&self) -> Result<u64> {
        if self.count == 0 {
            return Err(Error::NotFound);
        }
        Ok(self.next_seq - self.count as u64)
    }

    pub fn newest_seq(&self) -> Result<u64> {
        if self.count == 0 {
            return Err(Error::NotFound);
        }
        Ok(self.next_seq - 1)
    }

    pub fn contains(&self, seq: u64) -> Result<bool> {
        if self.count == 0 {
            return Ok(false);
        }
        let oldest = self.next_seq - self.count as u64;
        Ok(seq >= oldest && seq < self.next_seq)
    }

    /// Next identity to be handed out by [Self::append].
    pub fn next_seq(&self) -> u64 {
        self.next_seq
    }

    pub fn count(&self) -> usize {
        self.count
    }

    pub fn capacity(&self) -> usize {
        self.records.len()
    }
}
