# mstore — журнал реализации

> Журнал ведётся по мере работы. Архитектура — `MSTORE.md`.
> Здесь: текущий статус, план, хронология и принятые решения.

## 1. Статус

| | |
|---|---|
| Компонент | `mstore` (Table Store + Ring Store) |
| Архитектура | заморожена, `MSTORE.md` Ревизия 4 |
| Реализация | RAM core complete: Table + Ring; host-тесты зелёные, IDF/P4 сборка проходит |
| Расположение | `esp32-p4_HA_v2/mstore/` (в `shared_components/` — при втором потребителе) |
| Тестовые контуры | host (unit + randomized model tests) + ESP32-P4 test app |
| Осталось | hardware verification на реальной ESP32-P4 |

RAM core считается **функционально полным**; новые требования — только по результатам
прогона на железе/измерений, а не предположений.

## 2. Опорная модель

### Table Store

```text
canonical model:
    slots = meta + key + payload
runtime acceleration:
    index (linear probing + backward-shift), free-list, live_count
safe cached reference:
    slot + generation
change detection:
    version
logical lookup:
    key -> slot
mutation:
    slot + expected_generation
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

seq — долгоживущая identity записи (uint64, начинается с 1)
slot = (seq - 1) % capacity        // только физическое место
oldest = next_seq - count
newest = next_seq - 1

append -> перезапись oldest при полном ring
```

```text
seq ещё не существовал   -> NOT_FOUND   (seq == 0 или seq >= next_seq)
seq вытеснен из окна     -> STALE       (seq < oldest)
seq в окне               -> OK
пустой ring              -> oldest/newest -> NOT_FOUND
```

Ни index, ни generation, ни free-list у Ring нет.

## 3. Структура

```text
esp32-p4_HA_v2/mstore/
├── CMakeLists.txt                 # IDF-компонент: core + port/espidf
├── include/mstore/
│   ├── mstore_types.h             # mstore_err_t, mstore_meta_t, mstore_slot_t, schema
│   ├── mstore_table.h             # Table Store: slot-first API
│   └── mstore_ring.h              # Ring Store: append/seq API
├── src/
│   ├── mstore_platform.h          # внутренний контракт порта: allocator + lock
│   ├── mstore_internal.h          # Table: slot layout + состояние instance
│   ├── mstore_table.c             # Table: canonical slots: lifecycle + API
│   ├── mstore_runtime.c           # Table: derived index / free-list / live_count + rebuild
│   ├── mstore_invariants.c        # Table: check_invariants()
│   └── mstore_ring.c              # Ring: bounded окно + seq
├── port/
│   ├── host/mstore_platform_host.c      # malloc + CRITICAL_SECTION / pthread
│   └── espidf/mstore_platform_espidf.c  # heap_caps + FreeRTOS mutex
├── tests/                         # host build: unit + randomized model tests
│   ├── test_table.c
│   ├── test_model.c
│   ├── test_ring.c
│   └── test_ring_model.c
└── test_apps/mstore_p4/           # ESP-IDF smoke/integration: Table + Ring
```

## 4. План и прогресс

Table Store:

- [x] public types/API skeleton
- [x] internal table state + slot layout
- [x] host platform port
- [x] allocate / update / free / read / meta
- [x] runtime index / free-list / live_count
- [x] slot_find
- [x] rebuild_runtime
- [x] invariants
- [x] unit tests
- [x] randomized reference-model test

Ring Store:

- [x] public ring API + ring core
- [x] unit tests (границы: empty / 1 / capacity / capacity+1 / много оборотов)
- [x] randomized reference-model test

Интеграция:

- [x] ESP-IDF port
- [x] ESP32-P4 test app (Table + Ring)
- [ ] прогон на реальной ESP32-P4 (QEMU для esp32p4 в IDF не поддерживается)

`rebuild_runtime()` и `check_invariants()` — internal/debug, не публичный API; host tests
получают к ним доступ через `src/` private include.

## 5. Решения (decision log)

| Дата | Решение | Причина |
|---|---|---|
| 2026-09-30 | Заведён журнал реализации | фиксировать «делаем / собираемся делать» |
| 2026-09-30 | Компонент в `esp32-p4_HA_v2/mstore/`, не в `shared_components/` | один потребитель: держим рядом, шарим при втором |
| 2026-09-30 | Scope v1 — RAM-only Table Store | сначала доказать корректность slots, persistence позже |
| 2026-09-30 | Два контура тестирования: host + ESP32-P4 | host — алгоритм/скорость; P4 — интеграция embedded-кода |
| 2026-09-30 | Один core, `port/{host,espidf}`; platform выбирается при сборке | без vtable: compile-time port, ноль накладных |
| 2026-09-30 | `mstore_platform.h` — internal (`src/`), не публичный | caller его не использует |
| 2026-09-30 | Handle: `mstore_table_t/mstore_ring_t { void *_state; }` | внутренности скрыты, caller-owned |
| 2026-09-30 | Table `meta { used, generation, version }` — оба `uint32_t` | симметрично, без uint64 до необходимости |
| 2026-09-30 | `count` возвращает `mstore_err_t` + out-параметр | единая модель ошибок во всём API |
| 2026-09-30 | `rebuild_runtime` и `check_invariants` — internal | maintenance/debug, не пользовательский API |
| 2026-09-30 | Table index: linear probing + backward-shift; `index_capacity >= 2 * capacity` (load <= 0.5) | нет tombstones, не нужен rehash на этой load |
| 2026-09-30 | Ring `seq` — `uint64_t` с 1; `next_seq` = следующий выдаваемый | логическая identity, не зависящая от slot |
| 2026-09-30 | Ring без index/generation/free-list; `slot = (seq - 1) % capacity` | Ring проще Table; окно вычисляется из next_seq/count |
| 2026-09-30 | Ring: `seq >= next_seq`/0 → NOT_FOUND, `seq < oldest` → STALE | «не существовал» и «вытеснен» — разные исходы |
| 2026-09-30 | `check_invariants()` и model tests — часть v1 | больше уверенности, чем ручные unit-тесты |

## 6. Хронология

### 2026-09-30

- Архитектура `MSTORE.md` доведена до Ревизии 4 и зафиксирована.
- Заведён журнал; определён scope и согласована последовательность.
- Реализован host core Table Store: `mstore_table.c`, `mstore_runtime.c`,
  `mstore_invariants.c`, публичный API и host-порт.
- Реализован host core Ring Store: `mstore_ring.c`, публичный API.
- Добавлены IDF-порт, component `CMakeLists.txt` и ESP32-P4 test app (Table + Ring);
  сборка под ESP-IDF v6.1 / esp32p4 успешно проверена.

Результаты host-тестов (MSVC 14.51, `/W4`, warnings-as-errors):

```text
test_table       Passed
test_model       Passed   // 200000 random ops vs reference
test_ring        Passed   // границы: empty / 1 / capacity / capacity+1 / обороты
test_ring_model  Passed   // 300000 random ops vs reference
100% tests passed, 0 failed
```

Результаты IDF-сборки (ESP-IDF v6.1, target esp32p4):

```text
libmstore.a  собрана (Table + Ring + port/espidf)
mstore_p4.bin собран (test_apps/mstore_p4)
```

`idf.py qemu` для `esp32p4` IDF не поддерживает — запуск только на реальной P4.

Следующий шаг:

- RAM core complete, awaiting P4 hardware verification: flash `mstore_p4`, boot,
  прогон smoke suite (Table + Ring), проверка allocator/lock/lifecycle на железе;
- затем измерения: RAM footprint, latency `find/update`, стоимость lock, поведение при
  высокой capacity.

## 7. Открытые вопросы

1. Persistence: RAM-only или RAM+Flash; CRC/atomicity/wear/layout — после железа.
2. Table index: возможное изменение стратегии по результатам benchmark.
3. Ring: поведение при переполнении `uint64 next_seq` (wraparound) — сейчас не проектируется.
4. Lock и `iter`: контракт «колбэк не мутирует эту же таблицу» зафиксирован в заголовке.
5. `iter` vs `list`: `iter` в mstore, `list(filter)` в Domain.
