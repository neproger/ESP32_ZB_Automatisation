//! Snapshot records and the static /model.bin blob.

use super::*;

pub(super) fn snapshot_records(
    domain: &DomainHandle,
    state_only: bool,
) -> (u32, Vec<(u8, Vec<u8>)>) {
    let mut records: Vec<(u8, Vec<u8>)> = Vec::new();
    for schema in ha_web::SCHEMA {
        if state_only && schema.model {
            continue;
        }
        let Ok(snapshot) = domain.entity_snapshot(u32::from(schema.entity)) else {
            continue;
        };
        for index in 0..snapshot.len() {
            let (Some(key), Some(record)) = (snapshot.key(index), snapshot.record(index)) else {
                continue;
            };
            if key.len() != schema.key_size || record.len() != schema.record_size {
                log::warn!("web: schema mismatch type={}", schema.entity);
                continue;
            }
            let mut payload = Vec::with_capacity(key.len() + record.len());
            payload.extend_from_slice(key);
            payload.extend_from_slice(record);
            records.push((schema.entity, payload));
        }
    }
    // V2 `SNAPSHOT_STATE` sends zero counts; full snapshot sends the real count.
    let count = if state_only { 0 } else { records.len() as u32 };
    (count, records)
}

pub(super) fn enqueue_snapshot(fd: i32, domain: &DomainHandle, state_only: bool) -> bool {
    let (count, records) = snapshot_records(domain, state_only);
    let mut clients = CLIENTS.lock().unwrap();
    let Some(client) = clients.iter_mut().find(|client| client.fd == fd) else {
        return false;
    };
    if !enqueue_locked(client, control::SYNC_BEGIN, &count.to_le_bytes()) {
        return false;
    }
    for (frame_type, payload) in records {
        if !enqueue_locked(client, frame_type, &payload) {
            return false;
        }
    }
    enqueue_locked(client, control::SYNC_END, &count.to_le_bytes())
}
pub(super) fn build_model_blob(domain: &DomainHandle) -> Result<Vec<u8>, ()> {
    let mut out = Vec::new();
    out.try_reserve(12).map_err(|_| ())?;
    out.extend_from_slice(b"HASM");
    out.push(1); // WEB_SNAPSHOT_VER
    out.push(0);
    out.extend_from_slice(&0u16.to_le_bytes());
    let count_at = out.len();
    out.extend_from_slice(&0u32.to_le_bytes());
    let mut count = 0u32;
    for schema in ha_web::SCHEMA.iter().filter(|entry| entry.model) {
        let Ok(snapshot) = domain.entity_snapshot(u32::from(schema.entity)) else {
            return Err(());
        };
        for index in 0..snapshot.len() {
            let (Some(key), Some(record)) = (snapshot.key(index), snapshot.record(index)) else {
                return Err(());
            };
            if key.len() != schema.key_size || record.len() != schema.record_size {
                return Err(());
            }
            out.try_reserve(1 + key.len() + record.len())
                .map_err(|_| ())?;
            out.push(schema.entity);
            out.extend_from_slice(key);
            out.extend_from_slice(record);
            count = count.checked_add(1).ok_or(())?;
        }
    }
    out[count_at..count_at + 4].copy_from_slice(&count.to_le_bytes());
    Ok(out)
}
