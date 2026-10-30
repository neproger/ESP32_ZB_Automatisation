# Аудит V2 Flash / Region Manager / Ring

> Статус: **контракт Flash, Region Manager и Ring сверен по исходникам**. Код и
> `V3_IMPLEMENTATION_PLAN.md` не менялись.
>
> Эталон V2: `mstore/src/storage/mstore_region.{c,h}`,
> `mstore/src/storage/mstore_storage_flash.c`, `mstore/src/mstore_ring.c`,
> `mstore/include/mstore/mstore_ring.h`, `docs/storage/MSTORE_FLASH_REGIONS.md`,
> `MSTORE_FLASH_FORMAT.md`, тесты `mstore/tests/test_regions.c`,
> `test_flash*.c`, `test_composite.c`, `test_ring*.c`.
> Реализация V3: `crates/micro-db/src/flash.rs`, `ring.rs`, `ha-app/src/store.rs`.

## 1. V2 contract

### 1.1. Directory регионов

Раскладка раздела:

```text
offset 0                    directory slot A (1 erase block)
offset erase_size           directory slot B (1 erase block)
offset 2 * erase_size       region 0, region 1, … (bump, без дыр)
```

- Слот directory = 528 байт: header 16 + 16 записей × 32; слот занимает целый
  erase-блок, `erase_size >= 528`.
- Header: `magic MSDG u32@0`, `revision u16@4`, `max_entries u16@6`,
  `generation u32@8`, `CRC32@12` (над первыми 12 байтами).
- Запись: `persist_id[16]`, `offset u32@16`, `size u32@20`, `state u16@24`
  (`FREE=0`/`USED=1`), `reserved u16@26`, `CRC32@28` (над 28 байтами).
- `FREE` валидируется, но кодом не производится: запись либо `USED`, либо erased.
- Активен слот с большим `generation`; оба erased → `NOT_FOUND` + «первый commit
  идёт в слот 0»; ни одного валидного и есть non-erased данные → `CORRUPT`
  (без авто-формата).
- Commit: стереть неактивный слот, записать записи, **header последним**. Старый
  слот остаётся валидным до успешной записи нового header.

### 1.2. Размер и размещение региона

```text
max_record      = 16 + key_size + payload_size + 8
headroom        = max(capacity * 25/100, 8)
records_needed  = capacity + headroom
bank_size       = round_up(64 + records_needed * max_record, erase_size)
region_size     = 2 * bank_size
```

- `capacity ≤ u32`, `key_size/payload_size ≤ 0xFFFF`, `key_size > 0`.
- `erase_size >= 528`, раздел `>= 2 * erase_size`.
- Регион выровнен по `erase_size`, размер кратен `2 * erase_size`, регионы не
  пересекаются; содержимое раздела до `2 * erase_size` принадлежит directory.
- Адрес региона = `2 * erase_size + Σ size(USED)` (bump; дыры не переиспользуются,
  удаления региона в v1 нет).
- Смена геометрии: другой размер региона → `INVALID_SIZE`; тот же размер, но другой
  key/payload → отказ из bank header (`INVALID_STATE`), а не от Region Manager.

### 1.3. bind / release / lookup

- `persist_id = FNV-1a64(persist_key)` ×2 (те же seed'ы, что в bank header).
- `bind`:
  - нет directory → создать первым commit;
  - запись есть, размер совпал → отдать регион;
  - запись есть, размер другой → `INVALID_SIZE`;
  - запись есть, ключ уже занят в процессе → `INVALID_STATE`;
  - новый регион: directory заполнена → `NO_SPACE`; свободный хвост меньше →
    `INVALID_SIZE`; offset/size вне границ → `CORRUPT`;
  - успех: сначала запись directory, затем backend пишет в регион.
- Владение — RAM-таблица (16 id), не персистентна; `release` снимает только
  runtime ownership, directory entry остаётся `USED`;
  `owner_prune` снимает владение для id, которых нет в directory.
- `lookup` требует валидную directory; иначе `NOT_FOUND`.
- Регион отдаётся как view: `{base_offset, size}`, bounds-check, `erase_size`
  родителя; банки `bank = size / 2`.

### 1.4. Flash внутри региона

- Формат: header 64, record `16 + key + payload + 8`, magic/CRC/commit-marker,
  revision 1, `persist_id` в header; две банки, активна с большим `seq`.
- Fresh: нет валидных header'ов и header-области erased → стереть bank0, записать
  header `seq=1` (автоинициализация). Non-erased без header → `CORRUPT`.
- `header_matches`: `capacity`, `key_size`, `payload_size`, `persist_id` — **точное
  равенство**, иначе `INVALID_STATE`.
- Scan: читает записи до невалидной; committed-запись проверяет CRC и
  `slot < capacity`; torn tail (`run <= max_record`) — норма, помечает
  `needs_checkpoint`; прочий хвост — `CORRUPT`.
- Append: если `needs_checkpoint` или не хватает места → compact; ошибка program →
  `needs_checkpoint = true`. Запись: body+CRC, затем commit-marker последним.
- Compact: стереть неактивную банку, перенести последнюю запись каждого слота
  (включая tombstone), записать header `seq+1` последним, переключить банку,
  стереть старую best-effort.
- `clear_all`: `compact(clear=true)` — по каждому записанному слоту пишется META
  `used=false` с сохранением `generation`/`version`, затем header. Clear durable,
  generation не сбрасывается.
- `read_meta/read_key/read_slot` на слоте без записи возвращают `Ok` с
  `used=false`/нулями; `STALE` формирует Table Engine.
- `sync` — no-op; `close` освобождает latest/буферы и делает `region_release`.

### 1.5. Ring (`mstore_ring.c`, полный аудит)

- `seq` с 1, монотонный, физический слот `(seq-1) % capacity`; при заполнении
  перезаписывается самый старый.
- `get_by_seq`: `seq==0` или `seq >= next` → `NOT_FOUND`; вытесненный → `STALE`.
- `oldest/newest` на пустом → `NOT_FOUND`; `contains` вытесненного → `false`;
  `count`; `init` с `capacity/record_size == 0` или переполнением → `INVALID_SIZE`;
  повторный `init`/`deinit` → `INVALID_STATE`.
- Один lock на ring; записи фиксированного размера, копирование memcpy.
- Переполнение `next_seq` (u64) **не проверяется**: на `UINT64_MAX` счётчики
  переполнятся (граница, не покрытая тестами).

## 2. Что является деталью реализации, а не контрактом

- Bump-аллокатор без переиспользования дыр и порядок записей directory.
- RAM-таблица владельцев (`s_owner_ids`) и `owner_prune`.
- Отсутствие операции удаления региона/`FREE`-записи.
- `mstore_region_lookup` — тестовый хелпер.
- Запас `headroom` — решение реализации, но влияет на размер региона, поэтому
  переносится вместе с формулой.

## 3. V3 comparison

| # | Пункт | V2 | V3 | Тип |
|---|---|---|---|---|
| F1 | Region Manager | есть | нет; один раздел = одна таблица | **отсутствует (этап 1)** |
| F2 | Формат header/record/CRC/commit | 64/16+…/CRC32/MSCM | идентично | ✅ |
| F3 | `persist_id` и CRC32 | FNV×2, IEEE reflected | идентично | ✅ |
| F4 | Fresh-раздел | `open` создаёт header seq=1 | `open` → `Corrupt`; init в `store.rs` | расхождение (компенсировано) |
| F5 | Header geometry | точное равенство, иначе `INVALID_STATE` | capacity ≤ запрошенной (рост), ошибка `InvalidSize` | расхождение |
| F6 | Авто-format при `InvalidSize` | запрещён | `store.rs` форматирует раздел `domain` | **дефект (D22)** |
| F7 | `clear_all` | compact с META, generation сохраняется | отсутствует | **отсутствует** |
| F8 | Чтение слота без записи | default meta/нули, `Ok` | `read_slot` → `NotFound` | расхождение seam |
| F9 | Compact | copy latest → header → swap → erase old | то же | ✅ |
| F10 | Torn/corrupt recovery | classification по run/CRC/marker | то же | ✅ |
| F11 | `sync` | no-op | отсутствует | Rust-исключение |
| F12 | Ring | seq/STALE/NOT_FOUND/contains | то же | ✅ |
| F13 | Ring overflow u64 | не проверяется | `Overflow` | улучшение V3 |
| F14 | Ring lock | один lock | нет lock, Domain mutex | Rust-исключение |
| F15 | Регион/партиции | один `mstore`, регионы | `mstore` (registry) + `domain` раздельно | решение V3 |

## 4. Закрытые вопросы из `MICRO_DB_AUDIT.md`

- **`clear_all`**: разницы в generation между backend'ами нет — RAM ставит `used=false`,
  Flash пишет META `used=false`; generation/version сохраняются в обоих. Разница
  только в durability: Flash-clear переживает reboot. Рекомендация: портировать V2
  как есть, без нормализации.
- **`payload_size == 0`**: V2 допускает (проверяется только `key_size`); V3
  `FlashTable::format` отвергает. Решение открыто; склонность — сохранить V2.
- **`can_append`**: в V2 такой операции нет — место резервируется размером региона
  (`capacity + max(25%, 8)`), append сам делает compact. Рекомендация: убрать
  pre-check, полагаться на append+compact; размер региона считать по V2-формуле.
- **Владелец Region Manager**: в V2 — storage-слой `mstore`, в V3 — `micro-db`
  (этап 1). Подтверждено.
- **Физическая совместимость**: формат и `persist_id` совпадают; Device Registry
  читается. Раскладка Domain меняется на этапе 1 → нужен разовый erase.

## 5. Stage mapping

### Этап 0 (Flash backend внутри micro-db)

- Сохранить формат/CRC/commit/recovery/compaction/`persist_id` без изменений.
- `Storage` с `read_meta/read_key/read_slot/write_slot/write_meta/clear_all/sync`,
  чтения `&self`; lenient-чтение свободного слота (F8).
- `clear_all` по V2 (META, generation сохраняется, durable) (F7).
- Fresh: либо `open` сам инициализирует erased-раздел (V2), либо оставить init в
  composition root и зафиксировать это как решение (F4).
- Header geometry: решить F5 (точное равенство V2 или документированный рост).
- **Убрать авто-format при `InvalidSize`** в `store.rs`; при смене схемы —
  диагностика и отказ (F6).
- Ошибки: `InvalidSize`/`InvalidState` привести к V2-кодам (D25).
- Тесты: header/record parity, fresh, torn, corrupt, compact, clear durable,
  reopen, geometry mismatch, no auto-format.

### Этап 1 (Region Manager)

- Directory: 528-байтный слот, два erase-блока, header/entry CRC, generation,
  ping-pong, header последним.
- `bind/release/lookup`: persist_id, owner RAM, duplicate → `INVALID_STATE`,
  размер → `INVALID_SIZE`, directory полна → `NO_SPACE`, bounds → `CORRUPT`.
- Размер региона: `max(25%, 8)` headroom, `round_up` по erase, `2 * bank`.
- Region view: bounds-check, `erase_size`, `bank = size/2`.
- Per-entity таблицы Domain; разовый erase раздела `domain` (V2 §9.8).
- Registry остаётся в своём разделе как одна таблица (F15) — решение V3, без
  Region Manager, данные не меняются.

### Вне этапов 0–1

- Удаление/reuse регионов, дефрагментация, менеджер нескольких разделов.
- Миграция раскладки; авто-формат при повреждении.
- Изменение формата Flash-записей.

## 6. Tests

- Directory: fresh, ping-pong recovery (каждый слот активен), оба erased,
  не-erased без header → `CORRUPT`, CRC/state/entry corruption → `CORRUPT`.
- Bind: повторный bind при активном владении → `INVALID_STATE`; после `release`
  повторный bind того же ключа возвращает прежние `offset/size` (directory entry
  остаётся `USED`, bump tail не меняется); физического освобождения/переиспользования
  места нет; другой размер → `INVALID_SIZE`; directory full → `NO_SPACE`; free tail
  мал → `INVALID_SIZE`.
- Layout: offset выровнен, `>= 2 * erase`, размер кратен `2 * erase`, регионы не
  пересекаются, lookup стабилен после reopen.
- Geometry mismatch: тот же размер региона, другой key/payload → `INVALID_STATE`;
  другой размер → `INVALID_SIZE`; авто-формата нет.
- Clear: durable, generation сохраняется, reopen видит пустую таблицу.
- Compact/torn/corrupt: как в V2 `test_flash*`.
- Ring: seq/eviction/`STALE`/`NOT_FOUND`/`contains`/`count`; u64 overflow (V3).

## 7. Открытые вопросы

- Fresh-раздел: автоинициализация в `open` (V2) или в composition root (V3)?
- Header capacity: точное равенство (V2) или разрешить рост (V3)?
- Маппинг кодов ошибок geometry mismatch (`INVALID_STATE` vs `INVALID_SIZE`).
- Registry: оставить вне Region Manager (текущее V3) — подтвердить.
- `payload_size == 0` и `can_append` — см. §4.

## 8. Следующий слой

Domain Store: mutation, persistence policy, Delta, Journal, restore.
