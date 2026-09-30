# mstore — журнал реализации

> Журнал ведётся по мере работы. Архитектура — `MSTORE.md`.
> Здесь: текущий статус, план, хронология и принятые решения.

## 1. Статус

| | |
|---|---|
| Компонент | `mstore` (Table Store + Ring Store) |
| Архитектура | `MSTORE.md` (Ревизия 4 + storage backend, §2.11) |
| RAM core | complete: Table + Ring; host-тесты зелёные, IDF/P4 сборка проходит |
| Storage architecture | зафиксирована: один Table Engine + storage backend'ы; RAM extraction выполнен |
| Расположение | `esp32-p4_HA_v2/mstore/` (в `shared_components/` — при втором потребителе) |
| Осталось | storage backend extraction → flash format → hardware verification |

## 2. Опорная модель

### Table Store

```text
canonical model   = логический fixed-capacity массив slots
slot              = meta + key + payload
physical residence= storage backend (RAM / FLASH / RAM+FLASH)

derived runtime   = index (linear probing + backward-shift), free-list, live_count
safe cached ref   = slot + generation
change detection  = version
logical lookup    = key -> slot
mutation          = slot + expected_generation
```

```text
key не существует        -> NOT_FOUND
cached slot устарел      -> STALE
capacity закончилась     -> NO_SPACE
key уже существует       -> ALREADY_EXISTS
payload не изменился     -> OK + changed=false
payload изменился        -> OK + changed=true
```

### Ring Store

```text
records[capacity] + next_seq + count
seq — долгоживущая identity (uint64 с 1); slot = (seq - 1) % capacity
oldest = next_seq - count;  newest = next_seq - 1
append перезаписывает oldest при полном ring
seq >= next_seq / 0 -> NOT_FOUND;  seq < oldest -> STALE;  пустой ring -> NOT_FOUND
index / generation / free-list у Ring нет
```

## 3. Storage architecture (checkpoint)

Главное разделение:

```text
Table Engine     — ЧТО означает операция (вся семантика и runtime state)
Storage Backend  — ГДЕ и КАК физически лежат canonical slot bytes
```

```text
                    public mstore API
                           │
                    mstore_table facade
                           │
                    Table Engine
             semantics / lifecycle / runtime state
                           │
                    storage backend
               ┌───────────┼───────────┐
               │           │           │
              RAM        FLASH      RAM+FLASH
```

Table Engine владеет: key identity, slot lifecycle, generation, version,
expected_generation, `ALREADY_EXISTS / NOT_FOUND / STALE / NO_SPACE`, `changed`, index,
free-list, live_count, iter, count, clear, rebuild, invariants.

Backend владеет только canonical bytes и sync/commit. Backend не знает смысла key и
причины изменения generation/version.

Storage modes (bitmask в schema):

```text
RAM_ONLY   = MSTORE_BACKING_RAM
FLASH_ONLY = MSTORE_BACKING_FLASH
RAM_FLASH  = MSTORE_BACKING_RAM | MSTORE_BACKING_FLASH
```

Internal storage contract (private, имена уточняются по коду):

```text
open / init, close
read_meta(slot), read_key(slot), read_slot(slot)
write_slot(slot, state), write_meta(slot, meta)
clear_slot(slot), clear_all()
sync / commit
```

Публичный API не зависит от backend; наружу только `mstore_table_*`.

Планируемая структура:

```text
src/
├── mstore_table.c               # Table Engine (semantics)
├── mstore_runtime.c             # derived acceleration
├── mstore_invariants.c
├── mstore_ring.c
└── storage/
    ├── mstore_storage.h         # internal backend contract
    ├── mstore_storage_ram.c
    ├── mstore_storage_flash.c       # позже
    └── mstore_storage_ram_flash.c   # позже
```

## 4. План

Table/Ring RAM core — сделано:

- [x] Table Store RAM core + host tests
- [x] Ring Store RAM core + host tests
- [x] ESP-IDF port + P4 smoke app

Storage phase:

- [x] прочитать `MSTORE.md`, журнал, код `mstore`, старый `micro_db`
- [x] зафиксировать storage architecture в документации
- [x] выделить physical RAM access из Table Engine за internal storage contract
- [x] подключить RAM backend; host tests зелёные без смены reference-модели
- [x] host flash/NOR simulator + fault injection
- [x] спроектировать durable format (`MSTORE_FLASH_FORMAT.md`)
- [ ] FLASH backend (append-only log, две банки, checkpoint)
- [ ] RAM+FLASH composite backend
- [ ] общий behavioral/model suite на всех трёх storage modes
- [ ] reboot / power-loss / corruption tests
- [ ] ESP-IDF `esp_partition` backend
- [ ] единый mstore hardware suite на реальной P4

Порядок из архитектурного решения соблюдается: flash format проектируется только после
зелёного RAM backend extraction.

## 5. Решения (decision log)

| Дата | Решение | Причина |
|---|---|---|
| 2026-09-30 | Заведён журнал реализации | фиксировать «делаем / собираемся делать» |
| 2026-09-30 | Компонент в `esp32-p4_HA_v2/mstore/`, не в `shared_components/` | один потребитель: держим рядом |
| 2026-09-30 | Scope v1 — RAM-only | сначала доказать корректность slots |
| 2026-09-30 | Два контура: host + ESP32-P4 | host — алгоритм/скорость; P4 — интеграция |
| 2026-09-30 | Один core, `port/{host,espidf}`, compile-time port | без vtable, ноль накладных |
| 2026-09-30 | `mstore_platform.h` — internal, не публичный | caller его не использует |
| 2026-09-30 | Handle: `{ void *_state; }`, caller-owned | внутренности скрыты |
| 2026-09-30 | Table `meta { used, generation, version }` — оба `uint32_t` | симметрично |
| 2026-09-30 | `count` -> `mstore_err_t` + out | единая модель ошибок |
| 2026-09-30 | `rebuild_runtime` / `check_invariants` — internal | maintenance/debug |
| 2026-09-30 | Table index: linear probing + backward-shift; load <= 0.5 | без tombstones/rehash |
| 2026-09-30 | Ring `seq` — `uint64_t` с 1; без index/generation/free-list | Ring проще Table |
| 2026-09-30 | Ring: будущий/несуществующий seq -> NOT_FOUND, вытесненный -> STALE | разные исходы |
| 2026-09-30 | **Один Table Engine + storage backends**, не две реализации Table Store | убрать дублирование semantics |
| 2026-09-30 | Backing — bitmask в schema (RAM / FLASH / RAM\|FLASH) | отражает композицию |
| 2026-09-30 | FLASH-only поддерживается; RAM больше не обязателен | снимает ограничение v1 |
| 2026-09-30 | Публичный API не зависит от backing | caller не знает storage mode |
| 2026-09-30 | Durable format не фиксируется; сначала host simulator + fault injection | доказать recovery без P4 |
| 2026-09-30 | Ring не втягивать в persistence refactor | без преждевременной универсализации |
| 2026-09-30 | Storage contract: vtable + copy-out (`read_meta/read_key/read_slot`, `write_slot/write_meta`, `clear_all`, `sync`, `close`); `read_key` сохранён | FLASH reserve: `slot_find`/rebuild не читают payload |
| 2026-09-30 | `write_*` = logical canonical commit; derived runtime меняется только после успешного write | backend сам решает commit/recovery, engine без rollback |
| 2026-09-30 | `sync()` — внутренний backend flush, не public durability contract | caller не должен знать persistence policy |
| 2026-09-30 | NOR simulator — в `tests/support/`, не в компоненте; device seam для flash backend пока не вводится | нет потребителя в компоненте до durable format |
| 2026-09-30 | Flash format: две банки + append-only log + checkpoint (проект `MSTORE_FLASH_FORMAT.md`) | power-loss safety без erase на каждую мутацию; fixed-per-slot и dual-copy отвергнуты |
| 2026-09-30 | RAM-карта `slot -> latest offset` как bounded derived для FLASH | случайное чтение без полного payload в RAM |
| 2026-09-30 | `clear_all` = append `META(used=0)` на занятый слот (generation сохраняется) | совпадает с RAM backend |
| 2026-09-30 | Новые нейтральные ошибки `MSTORE_IO` / `MSTORE_CORRUPT` (архитектурная фиксация) | caller не видит RAM/flash-специфику |

## 6. Хронология

### 2026-09-30

- Архитектура `MSTORE.md` доведена до Ревизии 4 и зафиксирована.
- Реализован RAM core: Table Store и Ring Store, публичный API, host-порт.
- Host tests: `test_table`, `test_model`, `test_ring`, `test_ring_model` — зелёные.
- IDF-сборка ESP-IDF v6.1 / esp32p4 и P4 smoke app (Table + Ring) — проходят.
- Новый checkpoint: storage architecture (Table Engine + backends); `MSTORE.md`
  обновлён (§2.5, §2.10, §2.11), storage decisions зафиксированы в журнале.
- Выполнен extraction: Table Engine работает поверх internal storage contract
  (`src/storage/mstore_storage.h`, `mstore_storage.c`, `mstore_storage_ram.c`);
  публичный API и reference-модели не менялись (в schema добавлен только `backing`).
  Host-тесты и IDF-сборка — зелёные.
- Добавлен host NOR simulator (`tests/support/nor_sim.{h,c}`): erased `0xFF`, program
  только `1 -> 0`, erase с выравниванием, fault injection (partial program, fail now,
  fail erase), `poke`, save/load для reboot. Self-test `test_flash_sim` зелёный.
- Спроектирован durable format (`MSTORE_FLASH_FORMAT.md`): две log-банки, append-only
  записи `SET/META` с CRC32, RAM-карта `slot -> latest offset`, checkpoint через
  switch банки по `seq`. Реализация — следующий шаг.

Следующий шаг:

- durable format поверх ограничений симулятора: варианты layout, recovery и
  power-loss; выбрать по критериям (atomic observable state, bounded RAM, erase/write
  amplification, compaction, corruption detection).

## 7. Открытые вопросы

1. Durable flash format: append-only/journaled или иное; compaction, erase/write
   amplification, corruption detection — после host simulator.
2. RAM+FLASH commit policy: write-through vs batched; recovery/rollback внутри composite.
3. Storage contract: точный набор/гранулярность операций определится по коду extraction.
4. Ring persistence: общий storage abstraction или специализированный layout — после
   Table.
5. Table index: возможное изменение стратегии по результатам benchmark.
6. `iter` vs `list`: `iter` в mstore, `list(filter)` в Domain.
