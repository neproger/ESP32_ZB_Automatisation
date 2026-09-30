# mstore — журнал реализации

> Журнал ведётся по мере работы. Архитектура — `MICRO_DB.md`.
> Здесь: текущий статус, план, хронология и принятые решения.

## 1. Статус

| | |
|---|---|
| Компонент | `mstore` (Table Store first) |
| Архитектура | заморожена, `MICRO_DB.md` Ревизия 4 (`2e6a84b`) |
| Реализация | host core + тесты зелёные; IDF-компонент и P4 app собираются (esp32p4) |
| Расположение | `esp32-p4_HA_v2/mstore/` (в `shared_components/` — при втором потребителе) |
| Тестовые контуры | host (unit + randomized model test) + ESP32-P4 test app |

Архитектура Table Store считается **замороженной до результатов первой реализации**:
новые требования добавляются только если код покажет реальную дыру, а не предположение.

## 2. Опорная модель (не пересматривать без причины)

```text
canonical model:
    slots = meta + key + payload

runtime acceleration:
    index
    free-list
    live_count

safe cached reference:
    slot + generation

change detection:
    version

logical lookup:
    key -> slot

mutation:
    slot + expected_generation
```

Результаты операций:

```text
key не существует        -> NOT_FOUND
cached slot устарел      -> STALE
capacity закончилась     -> NO_SPACE
key уже существует       -> ALREADY_EXISTS
payload не изменился     -> OK + changed=false
payload изменился        -> OK + changed=true
```

## 3. Структура

```text
esp32-p4_HA_v2/mstore/
├── CMakeLists.txt                 # IDF-компонент: core + port/espidf
├── include/mstore/
│   ├── mstore_types.h             # mstore_err_t, mstore_meta_t, mstore_slot_t, schema
│   └── mstore_table.h             # публичный slot-first API
├── src/
│   ├── mstore_platform.h          # внутренний контракт порта: allocator + lock
│   ├── mstore_internal.h          # slot layout + состояние instance
│   ├── mstore_table.c             # canonical slots: lifecycle + API
│   ├── mstore_runtime.c           # derived: index / free-list / live_count + rebuild
│   └── mstore_invariants.c        # check_invariants()
├── port/
│   ├── host/mstore_platform_host.c      # malloc + CRITICAL_SECTION / pthread
│   └── espidf/mstore_platform_espidf.c  # heap_caps + FreeRTOS mutex
├── tests/                         # host build: unit + randomized model test
└── test_apps/mstore_p4/           # ESP-IDF app: CMakeLists / sdkconfig.defaults / main
```

Два контура: host — корректность алгоритма; ESP32-P4 — интеграция embedded-кода.
Core platform-independent C; ESP-IDF — только порт.

## 4. План и прогресс

- [x] структура компонента
- [x] два test target: host и ESP32-P4 (`test_apps/mstore_p4/`)
- [x] platform layer: host + espidf порты
- [x] `mstore_table_schema_t` (capacity, key_size, payload_size, опц. payload_equals)
- [x] slot layout `meta + key + payload`, фиксированный stride
- [x] accessors meta/key/payload
- [x] generation/version lifecycle (allocate/update/free/reuse)
- [x] runtime index (`key -> slot`; linear probing + backward-shift deletion)
- [x] free-list / live_count
- [x] `rebuild_runtime()` из slots
- [x] API: slot_find / slot_meta / slot_read / slot_allocate / slot_update / slot_free
- [x] iter / count / clear
- [x] `check_invariants()`
- [x] randomized model test vs reference-модель
- [x] собрать IDF-компонент и ESP32-P4 test app (ESP-IDF v6.1, target esp32p4)
- [ ] запуск на реальной P4 (QEMU для esp32p4 в IDF не поддерживается)

`rebuild_runtime()` и `check_invariants()` — internal/debug, не публичный API; host tests
получают к ним доступ через `src/` private include.

Согласованная последовательность работ:

```text
1. public types/API skeleton
2. internal table state + slot layout
3. host platform port
4. allocate/update/free/read/meta
5. runtime index/free-list/live_count
6. slot_find
7. rebuild_runtime
8. invariants
9. unit tests
10. randomized reference-model test
11. ESP-IDF port
12. ESP32-P4 test app
```

## 5. Решения (decision log)

| Дата | Решение | Причина |
|---|---|---|
| 2026-09-30 | Заведён журнал реализации | фиксировать «делаем / собираемся делать» |
| 2026-09-30 | Компонент в `esp32-p4_HA_v2/mstore/`, не в `shared_components/` | один потребитель: держим рядом, шарим при втором |
| 2026-09-30 | Scope v1 — RAM-only Table Store | сначала доказать корректность slots, persistence/Ring позже |
| 2026-09-30 | Два контура тестирования: host + ESP32-P4 | host — алгоритм/скорость; P4 — интеграция embedded-кода |
| 2026-09-30 | Один core, `port/{host,espidf}`; platform выбирается при сборке | без vtable: compile-time port, ноль накладных |
| 2026-09-30 | `mstore_platform.h` — internal (`src/`), не публичный | caller его не использует |
| 2026-09-30 | Handle: `mstore_table_t { void *_state; }` | внутренности скрыты, caller-owned |
| 2026-09-30 | `meta { bool used; uint32_t generation; uint32_t version; }` | симметрично, без uint64 до необходимости |
| 2026-09-30 | `count` возвращает `mstore_err_t` + out-параметр | единая модель ошибок во всём API |
| 2026-09-30 | `rebuild_runtime` и `check_invariants` — internal | maintenance/debug, не пользовательский API |
| 2026-09-30 | Индекс: linear probing + backward-shift deletion | нет tombstones, не нужен rehash при load <= 0.5 |
| 2026-09-30 | `check_invariants()` и model test — часть v1 | больше уверенности, чем ручные unit-тесты |

## 6. Хронология

### 2026-09-30

- Архитектура `MICRO_DB.md` доведена до Ревизии 4 и зафиксирована.
- Заведён журнал; определён scope и согласована последовательность.
- Реализован host core: `mstore_table.c`, `mstore_runtime.c`, `mstore_invariants.c`,
  публичный API и host-порт.
- Добавлены IDF-порт, component `CMakeLists.txt` и ESP32-P4 test app (сборка не проверена).

Результаты сборки и тестов (host, MSVC 14.51, `/W4`, warnings-as-errors):

```text
test_table  Passed
test_model  Passed   // 200000 random ops vs reference, rebuild каждые 137, invariants каждый op
100% tests passed, 0 failed
```

Результаты IDF-сборки (ESP-IDF v6.1, target esp32p4):

```text
libmstore.a       собрана (src/*.c + port/espidf)
mstore_p4.bin     собран (test_apps/mstore_p4)
```

`idf.py qemu` для `esp32p4` IDF не поддерживает — запуск только на реальной P4
(или вручную через QEMU, если понадобится).

Следующий шаг:

- запуск `mstore_p4` на реальной плате; затем возврат к остальным открытым вопросам.

## 7. Открытые вопросы (из архитектуры)

1. Аллокация: core выделяет сам (сейчас так) или принимает готовый буфер.
2. Lock и `iter`: колбэк не мутирует ту же таблицу (контракт зафиксирован в заголовке).
3. Runtime index: load factor / rebuild — сейчас backward-shift без порогов.
4. Persistence: RAM-only или RAM+Flash; CRC/atomicity/wear/layout — позже.
5. `iter` vs `list`: `iter` в mstore, `list(filter)` в Domain.
