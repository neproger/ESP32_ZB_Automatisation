# mstore — flash format (design)

> Рабочий артефакт. Статус: **proposal**. Format revision: **1 (draft)**.
> Проектируется поверх host NOR simulator (`tests/support/nor_sim`).
> Архитектура storage backends — `MSTORE.md` §2.11.

## 1. Идея

Логический slot **не имеет** фиксированного физического адреса во flash. Update =
append новой ревизии; старая ревизия становится garbage и физически удаляется при
checkpoint.

```text
Table Engine       logical fixed-capacity slots
      ↓
FLASH backend      append-only physical records
      ↓
latest[slot] -> physical offset
```

```text
SET slot=5 v1
SET slot=5 v2
SET slot=5 v3
latest[5] -> v3        // v1, v2 — obsolete/garbage, не история
```

Причина: NOR не допускает произвольный overwrite, `0 -> 1` только через erase, erase
крупными блоками, sector rewrite даёт wear/write amplification. Append в заранее
erased область естественно ложится на физику NOR.

## 2. logical slot != physical location

```text
RAM backend:    logical slot -> fixed RAM address
FLASH backend:  logical slot -> latest[slot] -> current log offset
```

После update физический offset меняется, логический slot — тот же. Это ключевое
различие backend'ов; Table Engine физический layout не знает.

## 3. Регион: две log-банки

```text
region
├── bank 0   header + append-only records
└── bank 1   header + append-only records
```

Активна банка с большим `seq` и валидным header CRC. `bank_size` кратен `erase_size`.

## 4. Bank header

```text
u32 magic
u16 format_revision
u16 header_size
u32 seq
u32 capacity
u16 key_size
u16 payload_size
u8  persist_id[16]
u32 crc32
```

Рекорды начинаются с выровненного `header_size`.

## 5. Record format (commit marker)

```text
u16 magic
u8  kind              // SET | META
u8  used
u32 slot
u32 generation
u32 version
[key]                 // только SET, key_size bytes
[payload]             // только SET, payload_size bytes
u32 crc32             // над всеми байтами записи до crc
u32 commit_marker     // пишется ПОСЛЕДНИМ
```

- `SET` — полное canonical state слота (`used=1`).
- `META` — только meta; в v1 используется для `used=0` (free / clear). Key/payload не
  пишутся: для свободного слота они не имеют семантики.

Порядок записи:

```text
1. program body + key/payload + CRC
2. program commit_marker ПОСЛЕДНИМ
3. committed = marker успешно записан
4. только после этого latest[slot] -> new offset
```

`commit_marker` отделяет оборванный append (power loss) от ранее committed, позже
повреждённой записи. Это и есть граница атомарности записи.

## 6. RAM-карта

`latest[capacity] = { offset, kind }` (offset == 0 → записи не было).

- `read_meta(slot)`: latest → meta (payload не читается).
- `read_key(slot)`: latest → key (только SET).
- `read_slot(slot)`: latest → meta + key + payload.
- нет в карте → default `{ used=0, generation=0, version=0 }`.

Карта bounded и восстанавливается сканированием банки. `generation` свободного слота
сохраняется через `META`, поэтому reuse даёт `generation+1` как в RAM.

## 7. Load / recovery

```text
read bank0.header, bank1.header
valid = crc ok and magic/revision ok
active = valid с максимальным seq
  нет валидных -> fresh: erase bank0, write header seq=1
scan active.records от header_size:

    fixed prefix не парсится (magic invalid / erased 0xFF)
        -> torn tail / конец лога -> stop

    parse kind/slot -> record length
    read commit_marker:
        marker отсутствует / partial / erased
            -> torn uncommitted tail -> stop        // норма, не ошибка
        marker valid:
            validate magic/slot/kind/crc32
                ok   -> latest[slot] = offset; advance
                bad  -> MSTORE_CORRUPT               // committed повреждён
```

Границы семантики:

```text
torn uncommitted tail      != MSTORE_CORRUPT   (нормальный recovery)
committed record corrupted  = MSTORE_CORRUPT
```

Ограничение: если у committed-записи повреждён сам fixed prefix (magic), marker
локализовать нельзя — такой случай классифицируется как torn tail. Это осознанный
компромисс: различаем committed corruption там, где структура ещё парсится.

## 8. Append и порядок derived state

```text
prepare new canonical record
    ↓
program body/CRC
    ↓
program commit_marker
    ↓ success
latest[slot] = new_offset
```

`latest` не переключается до подтверждённого commit. Внутри FLASH backend действует тот
же принцип, что в Table Engine: canonical write → затем derived.

## 9. Checkpoint (compaction)

```text
1. erase inactive bank
2. для каждого logical slot скопировать ТОЛЬКО current canonical state:
       latest SET  -> SET (meta/key/payload)
       latest META -> META (сохранить generation свободного слота)
       нет истории -> пропустить
3. program inactive.header { seq+1, persist_id, geometry }   // commit point
4. erase старую банку (lazily допустимо)
```

Checkpoint = garbage collection, а не история. Пример:

```text
до:   slot5 v1, slot5 v2, slot3 v1, slot5 v3, slot3 v2
после: slot5 v3, slot3 v2
```

Power-loss: до шага 3 активна старая банка; после — новая (`seq+1`), старая игнорируется.

## 10. clear_all (атомарный через новую банку)

Не append N записей (не атомарно). Используем механизм двух банок:

```text
1. erase inactive bank
2. для каждого slot с историей: META { used=0, generation=prev, version=prev }
3. program inactive.header { seq+1 }   // commit point всей операции
4. erase старую банку
```

```text
power loss до нового header -> старая таблица целиком активна
power loss после header    -> новая таблица целиком считается очищенной
```

`generation/version` сохраняются → семантика совпадает с RAM backend. Отдельный
`CLEAR_ALL` record пока не нужен.

## 11. Bank sizing

```text
bank_size >= header_size
           + capacity * max_slot_record_size
           + append_headroom

max_slot_record_size = fixed + key_size + payload_size + crc + commit_marker
region = 2 * bank_size
```

`append_headroom` нужен для новых ревизий до следующего checkpoint, а не потому что
таблица растёт (capacity фиксирована). Точный размер headroom — implementation
decision / benchmark.

## 12. FLASH_ONLY всё равно использует bounded RAM

FLASH_ONLY ≠ «0 RAM». В RAM остаются:

```text
latest[capacity]
index
free-list
live_count
scratch_key / scratch_payload
backend metadata (bank cursors, geometry)
```

Полного массива payload таблицы в RAM **нет** — это и есть цель FLASH_ONLY.

## 13. Persistent identity / geometry

`persist_key_hash` (32-bit) как identity недостаточен. Bank header несёт
`persist_id[16]` — широкий детерминированный digest полного `persist_key`. CRC32
остаётся только для integrity, не для identity.

```text
open:
    geometry mismatch (capacity/key_size/payload_size)
    или persist_id mismatch
        -> MSTORE_INVALID_STATE
        -> ничего автоматически не стирать
```

## 14. Error model

```text
MSTORE_IO       — physical read/program/erase failure
MSTORE_CORRUPT  — committed durable structure invalid
torn uncommitted tail != MSTORE_CORRUPT   (нормальный recovery scenario)
```

Семантика Table API для caller'а не меняется; ошибки storage не выходят наружу как
RAM/flash-специфика.

## 15. Что не меняем

Не возвращаться к:

```text
fixed physical slot per erase block
sector read-modify-erase-write на каждую мутацию
полный table rewrite на каждый update
```

Ring Store в persistence format пока не втягивать.

## 16. План реализации

1. зафиксировать format revision (этот документ);
2. FLASH backend поверх `nor_sim`;
3. базовый lifecycle;
4. recovery / reboot tests;
5. power-loss fault injection;
6. checkpoint / compaction;
7. atomic clear_all;
8. corruption tests;
9. общий behavioral suite: RAM, FLASH (позже RAM|FLASH);
10. только после зелёного host contour — ESP-IDF `esp_partition`.

Главный критерий: caller observable semantics одинаковы для RAM и FLASH; различается
только физический способ хранения.

## 17. Открытые вопросы

1. Точный размер `commit_marker` и его значение.
2. `append_headroom` по измерениям churn.
3. Алгоритм `persist_id[16]` (детерминированный digest).
4. Erase старой банки сразу vs lazily.
5. Composite RAM+FLASH: log backend + RAM-зеркало, commit policy.
