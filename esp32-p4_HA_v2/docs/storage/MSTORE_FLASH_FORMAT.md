# mstore — flash format (design)

> Рабочий артефакт. Статус: **proposal**, не реализовано.
> Проектируется поверх host NOR simulator (`tests/support/nor_sim`).
> Архитектура storage backends — `MSTORE.md` §2.11.

## 1. Что должен дать формат

Backend реализует internal storage contract (`read_meta/read_key/read_slot`,
`write_slot/write_meta`, `clear_all`, `sync`, `close`) поверх NOR-региона, при этом:

- `write_*` — logical canonical commit (успех = состояние принято целиком);
- power-loss safety и atomic observable state;
- recovery при старте;
- bounded RAM (для FLASH_ONLY полный массив payload в RAM не хранится);
- без erase/rewrite 4K-сектора на каждую мелкую мутацию (главный дефект v1);
- детекция повреждений (CRC), compaction, приемлемая write amplification.

## 2. Рассмотренные варианты

```text
A. Fixed per-slot region (v1-стиль)
   slot по фиксированному offset, у каждого CRC.
   - update требует erase сектора -> rewrite всех слотов в секторе;
   - окно потери старого значения на время erase;
   - write amplification = sector_size / slot_size.
   -> не подходит.

B. Dual-copy всей таблицы (shadow)
   две полные копии, пишем по очереди.
   - amplification = размер всей таблицы на каждую мутацию.
   -> не подходит.

C. Append-only log + RAM-карта slot -> offset
   мутации дописываются записями; старые значения становятся stale.
   - program без erase (erase только при compaction);
   - power-loss safe (CRC + последовательность);
   - случайное чтение через RAM-карту (bounded).
   -> выбирается.
```

## 3. Формат (принято к реализации)

Регион делится на **две равные log-банки**. В каждой банке — заголовок и
append-only поток записей. Активна банка с большим `seq` и валидным CRC.

```text
region
├── bank 0
│   ├── header   { magic, version, seq, capacity, key_size, payload_size,
│   │              persist_key_hash, crc32 }
│   └── records  (append-only)
└── bank 1
    ├── header
    └── records
```

`bank_size` кратен `erase_size` и вмещает: `header + snapshot + headroom`.

## 4. Запись (record)

Фиксированная часть + опциональные key/payload, в конце CRC32:

```text
u16 magic          // record magic
u8  kind           // SET | META
u8  used           // meta.used
u32 slot
u32 generation
u32 version
[ key     ]         // только SET, key_size bytes
[ payload ]         // только SET, payload_size bytes
u32 crc32           // над всеми байтами записи до crc
```

- `SET` — полное canonical состояние слота (`used=1`).
- `META` — только meta; в v1 используется для `used=0` (free). Key/payload
  отсутствуют: для свободного слота они не имеют семантики, писать их незачем.

## 5. RAM-карта

Backend держит `latest[capacity] = { offset, kind }` (offset == 0 = записи не было).

- `read_meta(slot)`: `latest` → запись → meta (payload не читается).
- `read_key(slot)`: `latest` → key (только SET).
- `read_slot(slot)`: `latest` → meta + key + payload.
- отсутствует в карте → default `{ used=0, generation=0, version=0 }`.

Карта — bounded (`capacity * sizeof(offset)+kind`) и полностью восстанавливается
сканированием банки. `generation` свободного слота сохраняется через `META`-запись,
поэтому reuse даёт `generation+1` как в RAM backend.

## 6. Load / recovery

```text
read bank0.header, bank1.header
valid = crc ok and magic/version ok
active = valid с максимальным seq
  нет валидных -> fresh: erase bank0, write header seq=1
scan active.records от header_size:
    read fixed part -> magic ok? crc ok? slot < capacity?
      ok  -> latest[slot] = offset; advance на длину записи
      нет -> stop (хвост считается невалидным)
live_count / free-list / index восстанавливает Table Engine через read_meta/read_key
```

## 7. Мутация (append)

```text
write_slot(slot, meta, key, payload):
    если не влезает в активную банку -> checkpoint()
    append SET-запись
    передать CRC и program
    успех -> latest[slot] = offset (derived после write)
write_meta(slot, meta):
    аналогично, kind=META (без key/payload)
```

Durability: program завершён и CRC валиден = запись durable (write-through). Публичный
`sync()` — no-op для этого backend'а.

## 8. Checkpoint (compaction)

Когда запись не влезает:

```text
1. erase inactive bank
2. append в inactive bank по одной записи на слот:
       latest SET  -> SET (meta/key/payload)
       latest META -> META (сохранить generation свободного слота)
       нет истории -> пропустить
3. последним program записать inactive.header { seq+1 }
       <-- это commit point; до него активна старая банка
4. erase старую банку (lazily допустимо)
```

Power-loss:

```text
до шага 3  -> активна старая банка (валидна)
после шага 3 -> активна новая (seq+1)  (старая игнорируется)
```

Банка обязана вмещать полный snapshot (`header + все слоты`), поэтому
`bank_size >= header + capacity * max_record`; регион = `2 * bank_size`.

## 9. clear_all

В v1: append `META(used=0)` для каждого занятого слота (generation сохраняется,
семантика совпадает с RAM backend). Альтернатива (checkpoint пустой таблицы) сбрасывает
generation и отвергнута из-за расхождения с RAM.

## 10. Error model

Семантика Table API для caller'а не меняется. Добавляются нейтральные значения для
storage-сбоев (архитектурная фиксация):

```text
MSTORE_IO       — device read/program/erase завершился ошибкой
MSTORE_CORRUPT  — CRC/структура невалидны
```

`check_invariants()` и так различает нарушение инварианта и ошибку storage.

## 11. Geometry / persist_key

Заголовок банки несёт `capacity`, `key_size`, `payload_size`, `persist_key_hash`.
При open:

```text
несовпадение geometry или persist_key_hash -> MSTORE_INVALID_STATE (без erase)
```

## 12. Тестовый план (на nor_sim)

- базовый lifecycle поверх FLASH backend, тот же behavioral suite, что для RAM;
- reboot после каждой мутации (reopen активной банки);
- power-loss на границах: partial program (fail after N), fail before/after commit,
  fail during erase/checkpoint;
- corrupt header / corrupt record / обрезанный хвост;
- checkpoint при заполнении банки;
- схемный mismatch и persist_key mismatch;
- после каждой recovery — `check_invariants()` зелёный.

## 13. Открытые вопросы

1. Точный `bank_size`/headroom (по измерениям churn).
2. Нужен ли отдельный `CLEAR_ALL` record вместо N `META`.
3. Composite RAM+FLASH: log backend + RAM-зеркало; commit policy.
4. CRC32 vs иной checksum; покрытие заголовка банки и записей.
5. Erase старой банки сразу vs lazily.
