# micro_db v1 — разбор перед реализацией v2

> Рабочий артефакт. Источник: `shared_components/micro_db` (core + flash).
> Цель — использовать v1 как **референс реализации**, а не как контракт v2.
> Категории: `KEEP` / `REWORK` / `DROP`.

## 1. Что есть в v1 (кратко)

**Core (`micro_db_core.c`):**

- `table` = schema + `records[]` (calloc на всю capacity) + `slot_used[]` + `free_slots[]`
  (LIFO-стек свободных слотов) + primary index (`primary_slots/primary_states/primary_keys`,
  pow2 capacity ≥ 2·N) + `live_count/free_count` + recursive mutex;
- primary index: FNV-1a, open addressing, tombstones (`EMPTY/USED/TOMBSTONE`);
- операции: `init/deinit`, `upsert`, `get`, `remove`, `clear`, `count`, `get_stats`,
  `get_slot`, `get_by_slot`, `get_by_index`, `iter`, `iter_slots`;
- upsert: `record_equals` → `changed`; вставка через free-list; rollback при неудачном
  persist (`release_inserted_slot`);
- free-list repair: при `free_count==0 && live<capacity` → `rebuild_runtime_state()` +
  `goto retry_lookup`;
- lock: `xSemaphoreCreateRecursiveMutex()` на таблицу; все операции под ним.

**Flash (`micro_db_flash.c`):**

- партиция `gw_data`; directory (до 16 таблиц: magic/version/entry_count/next_free_offset
  + entries с `persist_key[16]`, `region_offset/size`, `record_size`, `capacity`);
- регион таблицы (sector-aligned): `table_hdr` (magic/version/live_count) + `slot_used[]` +
  `slot_crc[]` + `records[]`;
- `patch_partition()` — read-modify-write с erase+write целого 4КБ сектора;
- per-slot CRC (FNV-1a); `write_meta/slot/slot_used`, `persist_table`, `clear_table`;
- загрузка: проверка header/magic/version/record_size/capacity/live_count, санитайз slot map,
  сверка `live_slots == hdr.live_count`, проверка CRC каждого живого слота → при ошибке
  `INVALID_RESPONSE` и очистка региона;
- compaction директории при нехватке места; глобальный singleton (`s_part`, `s_dir`, буферы,
  `s_flash_lock`).

## 2. KEEP (проверено практикой, подходит v2)

| Механизм | Почему |
|---|---|
| Primary index: FNV-1a + open addressing + tombstones | просто, предсказуемо, без аллокаций |
| Schema-колбэк `payload_equals` | решает, вырос ли `version`; домен-агностичность |
| Free-list LIFO: выделение/переиспользование слотов | O(1), доказано |
| Recursive mutex на instance | покрывает вложенные вызовы |
| Flash layout: directory + per-table region | масштабируется, sector-aligned |
| Magic/version header и проверка при загрузке | ловит несовместимость/повреждение |
| Per-slot CRC | точечно ловит битый слот |
| Санитайз slot map + сверка live_count + CRC при load | рабочая recovery |
| Corrupt → clear region | безопасный деградированный старт |
| Rollback RAM при неудачном persist | не оставляет расхождение RAM/flash |
| Free-list repair из slot map | восстанавливает консистентность |
| Валидация аргументов init + cleanup paths | надёжный жизненный цикл |
| Callback-итерация (`iter`/`iter_slots`) | привычный паттерн обхода |
| Стек-key с защитой размера | ловит негабаритный key |

## 3. REWORK (идея годная, реализация/контракт старые)

| Что | Проблема v1 | Направление v2 |
|---|---|---|
| `slot_used[]` отдельным массивом | meta не часть записи | свернуть в per-record `meta {used,generation,version}` |
| `key_of / key_equals` callback'и | key фактически часть payload; mstore зависит от layout клиента | key принадлежит storage, хранится в слоте (fixed layout), сравнение внутри |
| `record / record_size / record_equals` | «record» = payload | переименовать в `payload*` |
| Persisted per-slot meta | v1 хранит только `used`+CRC | добавить `generation/version` в meta и в checksum |
| Синхронный write-through на каждый upsert/remove | тяжёлая запись (erase+write 4КБ на патч) | определить политику persistence явно |
| Flash singleton + `MAX_TABLES=16` | глобальное состояние, жёсткий лимит | instance/конфигурируемость |
| `persist_key[16]` | фиксированная длина | параметризовать/ограничить явно |
| `void *lock` + касты | header без FreeRTOS, но грязно | нормальный opaque-тип |
| `iter` под lock | возможна реентрантность | зафиксировать контракт (не мутировать ту же таблицу) |
| Индекс без rehash | растут tombstones, растёт latency | порог rehash по доле tombstone |
| `goto retry_lookup` как штатный путь | defensive band-aid | сделать инвариант явным, repair — отдельно |

## 4. DROP (костыли старой архитектуры)

| Что | Почему |
|---|---|
| `get_by_index` (O(n) скан) | не входит в ядро v2; добавятся под потребителя |
| `get_stats` | не входит в ядро v2 |
| `slot_used[]` как отдельная сущность | заменяется `meta` внутри слота |
| статус/публичный `stats` | ядру v2 не нужен |

## 5. Ring Store

В v1 **Ring нет** — это полностью новый primitive. Опираться только на design rules
(`../storage/MICRO_DB.md`): seq как identity, overwrite-oldest, без key/hash/free-list.

## 6. Что это значит для §9 (mstore)

1. **Аллокация** → KEEP v1: `calloc` на capacity + free-list LIFO. Проверено.
2. **Lock/итерация** → KEEP recursive mutex на instance; `iter` под lock с явным
   контрактом «колбэк не мутирует эту же таблицу» (либо позже snapshot).
3. **Persistence** → REWORK: layout/CRC/recovery сохранить, добавить `generation/version`
   в persisted meta и в checksum; политику записи (write-through vs batched) зафиксировать.
4. **`iter` vs `list`** → KEEP `iter` в mstore; `list(filter)` собирает Domain.

## 7. Открытое после обзора

- покрытие checksum: только payload или `meta + key + payload`;
- нужна ли Ring-персистентность (в v1 аналога нет);
- политика rehash primary index;
- write-through vs batched persistence.
