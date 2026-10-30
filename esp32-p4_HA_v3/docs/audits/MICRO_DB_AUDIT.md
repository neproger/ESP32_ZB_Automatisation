# Аудит `micro-db`: Table Engine, Storage Backend, Ring

> Статус: **аудит контракта и семантики завершён; физический layout Flash и
> Region Manager — следующий слой**. Код и план не менялись.
>
> Эталон V2: `esp32-p4_HA_v2/mstore/include/mstore/*.h`,
> `mstore/src/*.c`, `mstore/src/storage/*.c`, `docs/storage/MSTORE.md`,
> `MSTORE_FLASH_FORMAT.md`, `MSTORE_FLASH_REGIONS.md`, тесты `mstore/tests/*`.
> Реализация V3: `crates/micro-db/src/{table,flash,persisted,ring}.rs` и потребители
> (`ha-domain`, `ha-app/store.rs`, `zigbee-devices/registry.rs`).

## 1. V2 contract

### 1.1. Публичный API Table (`mstore_table.h`)

| Операция | Семантика | Ошибки |
|---|---|---|
| `init` | валидирует schema, открывает backend, восстанавливает runtime из storage | `INVALID_SIZE`, `INVALID_ARG`, `INVALID_STATE`, `IO`, `CORRUPT`, `NO_MEM` |
| `deinit` | освобождает; повторный → ошибка | `INVALID_STATE` |
| `slot_find(key)` | `key → slot` через индекс, не сканом | `NOT_FOUND` |
| `slot_meta(slot)` | meta только для used; free slot → ошибка | `STALE`, `INVALID_ARG` |
| `slot_read(slot)` | snapshot `meta+key+payload` под одним lock; free → ошибка | `STALE`, `INVALID_ARG` |
| `slot_allocate(key,payload)` | дубль ключа → ошибка; нет места → ошибка; generation исчерпана → ошибка | `ALREADY_EXISTS`, `NO_SPACE`, `OVERFLOW` |
| `slot_update(slot,gen,payload)` | `changed=false`, если payload равен; version растёт только при изменении; version max → ошибка | `STALE`, `OVERFLOW` |
| `slot_free(slot,gen)` | проверка generation, `used=false` | `STALE`, `INVALID_ARG` |
| `iter(cb)` | обход used слотов под lock; буферы живут только на время вызова | `INVALID_ARG` |
| `count` | число used | — |
| `clear` | `storage.clear_all` + runtime reset | ошибки storage |

Источники поведения: `mstore_table.c:155-392`, `tests/test_behavior.c:44-112`,
`tests/test_boundaries.c`, `tests/test_model.c`.

### 1.2. Meta, slot lifecycle, derived state

- `meta = {used, generation, version}`; `generation` растёт при каждом reuse слота,
  `version` — только при фактическом изменении payload.
- `generation == UINT32_MAX` слот больше не переиспользуется (`OVERFLOW`).
- `version == UINT32_MAX` + реальное изменение → `OVERFLOW`, без изменения → `Ok(false)`.
- Engine меняет derived state (index/free-list/live_count) **только после** успешного
  `write_*` backend'а (`mstore_storage.h:19-27`).
- Индекс `key → slot` (capacity `next_pow2(capacity*2)`), free-list, live_count —
  производные, пересобираются из storage.
- `slot_meta(free)` = `STALE`; out-of-range slot = `INVALID_ARG`.

### 1.3. Backing

| Backing | Чтение | Запись | Восстановление |
|---|---|---|---|
| `RAM` | RAM | RAM | нет |
| `FLASH` | Flash | Flash | при `init` |
| `RAM\|FLASH` | RAM | **Flash commit → RAM** | при `init` из Flash |

- Composite: ошибка Flash → RAM не меняется, откат не нужен
  (`mstore_storage_ram_flash.c:1-10`); при `init` состояние берётся из Flash
  (`tests/test_composite.c`).
- `clear_all`: RAM — `used=false` с сохранением generation; Flash — compact в пустую
  банку (generation сбрасывается) (`mstore_storage_ram.c`, `flash_clear_all`).
- `sync` — backend lifecycle, not a durability contract.

### 1.4. Schema

`capacity`, `key_size`, `payload_size`, `payload_equals`, `backing`, `persist_key`.
`micro-db` не знает семантику key/payload. Для FLASH обязателен `persist_key`
(проверяется Domain, не storage). `key_size/payload_size ≤ 0xFFFF` в Flash-заголовке.

### 1.5. Границы со слоем Flash (подтверждено, детали — следующий слой)

- Header: 64 байта, `magic MSTB`, revision 1, `seq`, `capacity`, `key_size`,
  `payload_size`, `persist_id[16]`, CRC32; две банки, активна с большим `seq`.
- Record: `fixed 16` (`magic RT`, kind, used, slot, generation, version) +
  key/payload + `tail 8` (CRC32 + commit marker `MSCM`).
- `persist_id` = два FNV-1a64 от `persist_key` (seed `1469598103934665603` и
  `1099511628211`), CRC32 IEEE (reflected, `0xEDB88320`) — **байт-в-байт совпадает
  с V2**; Device Registry V3 читается после перехода на новый engine.
- Recovery: fresh erased → header seq=1; не-erased без валидного header → `CORRUPT`
  без авто-формата; torn-tail — нормальный recovery; повреждённая committed-запись —
  `CORRUPT`.
- Region Manager (V2): один раздел, directory из двух ping-pong erase-блоков
  (16 записей), один регион на `persist_key`, размер
  `bank = round_up(64 + (capacity + headroom) * max_record, erase)`,
  `region = 2 * bank`, `headroom = max(25%, 8)`; bind/release, повторный bind →
  `INVALID_STATE`, рост/несовпадение geometry → `INVALID_SIZE`, directory полна →
  `NO_SPACE`, повреждена → `CORRUPT`; миграции нет (`MSTORE_FLASH_REGIONS.md` §9.8).

### 1.6. Ring (`mstore_ring.h`)

- `seq` начинается с 1, монотонный, не переиспользуется; при заполнении вытесняется
  самый старый.
- `get_by_seq`: вытесненный → `STALE`, несуществующий (0 или > newest) → `NOT_FOUND`.
- `oldest/newest` на пустом → `NOT_FOUND`; `contains` вытесненного → `false`;
  `count`; дефолтные `INVALID_STATE`/`INVALID_SIZE` для невалидного lifecycle.
- Фиксированные записи; index/generation/free-list у Ring нет.

### 1.7. Инварианты и память

- Аллокации только при init/open; на hot path — preallocated scratch.
- Один lock на table; callback iter выполняется под ним.
- `mstore_check_invariants`: index ↔ slot ↔ meta, free-list без дублей,
  `free + live == capacity`; различает `INVARIANT_FAILED` и ошибку storage.

## 2. V3 implementation (до этапа 0)

- `Table<K,V>` — engine + RAM-хранение в одном типе; typed key/value, `Vec` на слот;
  индекс/backward-shift, generation/version, `changed`, `iter`, `clear`, `rebuild`;
  тесты: allocate/update/stale/reuse/overflow/index/clear/iter/ring.
- `FlashTable` — журнал с двумя банками, header/record/CRC/commit, recovery,
  auto-compaction, `read_slot`, `append_set/append_meta`; тесты: reboot, torn,
  corrupt committed, NoSpace, geometry mismatch, compaction, crash-сценарии.
  Константы формата и `persist_id` совпадают с V2.
- `PersistedTable` — композиция RAM+Flash: pre-check места, RAM mutation, append,
  откат RAM при ошибке; `open/format/get/meta/put/remove/compact`; тесты: reboot,
  tombstone, unchanged, failure-undo, NoSpace, restore free-list, capacity growth,
  wrong persist_id.
- `Ring<T>` — seq/eviction/`Stale`/`NotFound`/`contains`/`count`; требует `Clone`.
- Потребители: `ha-domain` (`Table`, `Ring`), `store.rs` и `registry.rs`
  (`PersistedTable`).

## 3. Differences и дефекты

| # | Пункт | V2 | V3 | Тип |
|---|---|---|---|---|
| D1 | `meta(free slot)` | `STALE` | возвращает `used=false` | **дефект контракта** |
| D2 | `clear` | RAM сохраняет generation, Flash сбрасывает | engine сбрасывает generation для всех | расхождение |
| D3 | `clear_all` в backend | есть | нет | **отсутствует** |
| D4 | Порядок composite | Flash → RAM | RAM → Flash + rollback | **расхождение** |
| D5 | Ошибка отката RAM | не нужна | проглатывается (`let _`) | **дефект** |
| D6 | `read_key` | есть | нет (читается весь слот) | отсутствует |
| D7 | Чтение free/missing слота в Flash | default meta/нули, `Ok` | `NotFound` | расхождение seam |
| D8 | Invariant checker | есть | нет | отсутствует (тест-утилита) |
| D9 | `payload_size == 0` | допустим | Flash отвергает | расхождение |
| D10 | `payload_equals` | опциональный компаратор | только `PartialEq`/memcmp | Rust-исключение (потребителей нет) |
| D11 | `sync`, `close` | есть в ops | нет / Drop | Rust-исключение |
| D12 | Iteration | callback под lock | borrow-iterator | API-адаптация |
| D13 | Region Manager | есть | нет | **отсутствует (этап 1)** |
| D14 | Flash-only backend | есть, но не используется Domain | нет | отложено |
| D15 | Append preflight | headroom в размере региона | `can_append` по live-count может отказать до compact | расхождение |
| D16 | `init` восстанавливает из Flash | да | `PersistedTable::open` | ✅ эквивалент |
| D17 | Индекс/free-list/generation/version | как контракт | как контракт | ✅ |
| D18 | Ring семантика | seq/Stale/NotFound/contains | та же | ✅ |
| D19 | persist_id/CRC/формат-константы | — | совпадают | ✅ |
| D20 | Журнал `PersistedTable::put` | `latest[]` O(1) | скан `journal` O(capacity) | перф, minor |
| D21 | Комментарий в `flash.rs` | — | «No bank rotation or compaction yet» при наличии compact | doc-дефект |

Устаревших тестов, противоречащих заголовкам, в `mstore` не найдено. В
`MSTORE_FLASH_FORMAT.md` статус «proposal / revision 1 (draft)», хотя формат
реализован и revision 1 зафиксирован — устаревшая метаданные документа, не контракт.

## 4. Required behavior

1. Один engine владеет слотами и derived state; backend владеет только байтами
   `meta+key+payload`; engine меняет derived state после успешного `write_*`.
2. Чтения `&self`, записи `&mut self`; `meta(free)` → `STALE`; out-of-range →
   `InvalidArg`.
3. Composite: Flash commit → RAM; ошибка Flash не меняет ни RAM, ни durable state.
4. `clear_all` — операция backend'а; engine после неё только пересобирает derived
   state (сохраняет backend-разницу generation).
5. Формат Flash и `persist_id` не меняются; Device Registry читается.
6. Ring остаётся отдельным механизмом с текущей семантикой.
7. `payload_equals` не переносится, пока нет потребителя; сравнение — memcmp.
8. Zero-copy `acquire/release` не входит в контракт (V2 `MSTORE.md:330`).

## 5. Stage mapping

- **Этап 0 (micro-db):**
  - `Storage` seam из 7 операций V2 (`read_meta`, `read_key`, `read_slot`,
    `write_slot`, `write_meta`, `clear_all`, `sync`) + `NorFlash::read(&self)`;
  - байтовый `Table` (opaque key/payload, geometry из descriptor), `Ram`, `Flash`,
    `RamFlash`; удаление `PersistedTable`/`Fixed`;
  - Flash-first write-through, удаление rollback-ветки;
  - `meta(free) → Stale`, `clear_all`, `read_key`, lenient-чтение free-слота;
  - invariant checker (test-only), callback iter;
  - D1–D9, D15–D19 (кроме региона).
- **Этап 1 (region):**
  - Region Manager: directory ping-pong, bind/release, размер региона с headroom
    `max(25%, 8)`, выравнивание `2 * erase_size`, ошибки, изоляция;
  - per-entity таблицы Domain, разовый erase раздела `domain` (V2 §9.8).
- **Вне этапов 0–1:** flash-only backend (до потребителя), `payload_equals`,
  zero-copy, многопоточная блокировка (V2 — один lock, V3 — Domain mutex).

## 6. Tests

Портировать/добавить:

- behavior-suite для всех трёх backing (allocate/update/free/stale/changed/count/
  iter/clear) — V2 `test_behavior.c`;
- `meta(free) → Stale`, out-of-range → `InvalidArg`;
- generation/version overflow — V2 `test_boundaries.c`;
- `RamFlash`: ошибка Flash → RAM и reopen без изменений; ошибка `clear` → состояние
  сохранено; `clear` переживает reopen — V2 `test_composite.c`,
  `test_flash_durability.c`;
- clear на RAM сохраняет generation, на Flash сбрасывает;
- model-based случайные операции с reboot'ами — V2 `test_model.c`,
  `test_flash_model.c`;
- format parity: header/record офсеты, CRC, `persist_id`-байты, revision;
- wrong persist_id, geometry mismatch, corrupt committed, torn tail (частично есть);
- invariant checker после каждой операции (V2 `mstore_check_invariants`);
- Ring parity: seq/eviction/`Stale`/`NotFound`/`contains`/overflow.

## 7. Открытые вопросы

- `clear_all`: сохранить разницу backend'ов (V2) или нормализовать? Рекомендация —
  сохранить V2.
- `payload_size == 0`: поддержать как V2 или оформить Rust-исключение?
  Потребителей сейчас нет.
- `can_append`: оставить pre-check или удалить и полагаться на append+compact?
  Размер региона всё равно считается по V2-формуле.
- Владелец Region Manager: `micro-db` (как V2) — подтвердить до этапа 1.
- Физическая совместимость: формат и `persist_id` совпадают, но раскладка Domain
  меняется на этапе 1 — решение erase/миграция остаётся за этапом 1.

## 8. Следующий слой

Flash / Region Manager: полный аудит формата, recovery, compaction, directory,
bind/release и раскладки разделов. Затем Domain Store (mutation/Delta/Journal).
