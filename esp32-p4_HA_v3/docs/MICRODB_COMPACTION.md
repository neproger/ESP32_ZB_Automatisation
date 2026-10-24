# MicroDB V2 bank/compaction audit

Source: `esp32-p4_HA_v2/mstore/src/storage/mstore_storage_flash.c` (verified V2),
`mstore/tests/test_flash_durability.c`.

## Layout

A region is **two banks**. Each bank:

```
[0..64)                  header
[64..bank_size)          append log of records
```

Header (`MSTB`, 64 bytes): magic u32, format revision u16, header size u16,
**sequence u32**, table capacity u32, key size u16, payload size u16, persist id
16 B, CRC32 over the first 36 bytes. Only a header with valid magic, revision,
size, geometry and CRC is usable; `open` also requires the persist id to match.

Bank size includes headroom: `records_needed = capacity + max(25% capacity, 8)`,
rounded up to the flash erase size. This makes compaction infrequent.

Record: `magic u16 | kind u8 | used u8 | slot u32 | generation u32 | version u32
| key | payload | CRC32 | commit_marker u32`. Kind is SET or META (meta has no
key/payload). The **commit marker is programmed separately, after the rest**, and
CRC covers everything before the marker. A record is visible only when the
marker is present; a torn tail is not applied.

## Open / recovery

1. Read both headers; pick the valid one with the higher `sequence`.
2. If neither is valid: both header regions must be fully erased, then format
   bank 0 with sequence 1; otherwise the region is corrupt.
3. Scan the active bank forward. Stop at the first missing/invalid record magic,
   missing commit marker, or truncated record. The tail is classified as:
   fully erased (clean), one non-erased run ≤ max record (torn append), anything
   else (corrupt). A torn tail sets `needs_checkpoint`; the next append compacts
   before writing.
4. `latest[slot]` points at the last committed record; the slot number,
   generation and version are kept in the record, so they survive compaction.
5. A record with `used = false` (META/tombstone) remains the latest for its slot
   and is copied by compaction: a removed device must not reappear.

## Compaction (commit order)

1. Erase the **inactive** bank.
2. Copy the latest record of every slot (skip slots never used). Keep slot,
   used, generation, version. SET copies key+payload; META copies header only.
   Every copied record is written with its commit marker last.
3. Write the new header (sequence + 1) into the inactive bank **last**. This is
   the commit point.
4. Switch the in-RAM active bank and sequence.
5. Erase the old bank (best effort; failure is ignored).

## Power-loss guarantees (as implemented by V2)

| Crash point | State after reboot | Data |
| --- | --- | --- |
| during erase of inactive | old header is still newest | old committed state |
| during record copy | inactive has no valid header (or old-seq header) | old bank selected |
| during new header write | header CRC fails (partial) | old bank selected |
| after new header, before RAM switch | new header has higher seq | new bank selected; all records were committed before the header |
| during old-bank erase | new bank valid | new bank selected |

There is no separate "atomic rename": the header CRC and higher sequence are the
commit. A record's marker is its per-record commit. V2 does **not** claim that a
crash inside a single record write is repaired; it is ignored as a torn tail.

## Mapping to V3 `micro-db/src/flash.rs`

The Rust backend already matches the V2 header and record envelope (magic,
revision, sizes, CRC, commit marker, SET/META, latest-per-slot, torn tail,
generation/version). It is currently one bank using the whole partition.

Planned extension, keeping `PersistedTable`'s API:

- `bank_size = capacity / 2`, aligned; bank 0 starts at 0, bank 1 at `bank_size`.
  Existing mstore data lives at offset 0 and stays in bank 0; bank 1 is erased,
  so the switch needs no data migration.
- `FlashTable` gains `active_bank`, `active_seq`, per-bank `write_offset`;
  `latest[].offset` stays **relative to the bank** (V2 uses 0 = none).
- `open` reads/validates both headers, selects the higher sequence, scans the
  active bank, sets `torn`.
- `append_*` compacts first when `torn` or when the record does not fit; a failed
  program marks the journal torn.
- `can_append` must account for compaction (the live set must fit one bank);
  otherwise keep returning `NoSpace` explicitly.
- `format` writes header sequence 1 in bank 0 and leaves bank 1 erased.
- The store's "all 0xFF means unformatted" check must consider both headers
  before calling `format`; a half-erased bank is not a valid new table.

## Host tests before mstore integration

Use a NOR simulator wrapper that fails on the Nth write/erase and can tear a
write after a chosen byte count, then reopen the same image:

1. crash during copy after k of N records → old bank's latest state;
2. crash during the new header (partial / bad CRC) → old bank;
3. crash after the header, before old-bank erase → new bank, same data;
4. crash during old-bank erase → new bank;
5. update/remove/crash/compact → tombstones stay removed, slot/generation/
   version unchanged;
6. fill the journal to the compaction threshold and append again → compaction,
   then normal appends continue.

Only after these pass, wire the 2-bank backend to `mstore`.
