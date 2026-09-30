# mstore — журнал реализации

> Журнал ведётся по мере работы. Архитектура — `MICRO_DB.md`.
> Здесь: текущий статус, план, хронология и принятые решения.

## 1. Статус

| | |
|---|---|
| Компонент | `mstore` (Table Store first) |
| Архитектура | заморожена, `MICRO_DB.md` Ревизия 4 (`2e6a84b`) |
| Реализация | не начата |
| Scope первой ревизии | RAM-only Table Store: без persistence, без Ring Store |
| Тестовые контуры | host (unit / randomized / sanitizers) + ESP32-P4 test app |

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

## 3. Scope первой ревизии

Только RAM-модель Table Store, platform-independent C. **Два контура тестирования
с первого коммита**: core не должен быть «формально platform-independent», проверяясь
только на Linux.

```text
mstore core
├── host build
│   ├── unit tests
│   ├── randomized model test
│   ├── check_invariants()
│   └── sanitizers / valgrind
│
└── ESP32-P4 test app
    ├── собирается ESP-IDF
    ├── гоняет тот же API
    ├── проверяет allocator/lock integration
    └── запускается на emulator или реальной P4
```

Разделение ответственности: host — корректность алгоритма; ESP32-P4 — корректность
интеграции embedded-кода. Третью среду (ESP-IDF `target=linux`) не вводим в первой
ревизии: механизм экспериментальный, поддержка компонентов ограничена.

Структура компонента — один core, разные platform ports:

```text
mstore/
├── include/
├── src/
├── port/
│   ├── host/
│   └── espidf/
└── tests/
```

```text
test_apps/
└── mstore_p4/
    ├── CMakeLists.txt
    ├── sdkconfig.defaults
    └── main/
        └── test_mstore.c
```

```text
mstore_table
├── slot memory/layout
├── meta/key/payload access
├── generation/version lifecycle
├── runtime index
├── free-list
├── live_count
├── rebuild_runtime
└── invariants/tests
```

Вне scope: flash/persistence, CRC/checksum, Ring Store, partition. Тонкий platform port
(allocator + lock) — в scope; полноценная интеграция ESP-IDF — нет.

## 4. План первой ревизии

- [ ] структура компонента `mstore` (`include/ src/ port/ tests/`)
- [ ] два test target: host и ESP32-P4 (`test_apps/mstore_p4/`)
- [ ] минимальный platform layer для обоих портов (allocator + lock)
- [ ] `mstore_table_schema_t` (capacity, key_size, payload_size, опц. payload_equals)
- [ ] slot layout: `meta + key + payload`, фиксированный stride
- [ ] accessors meta/key/payload
- [ ] generation/version lifecycle (allocate/update/free/reuse)
- [ ] runtime index (`key -> slot`, bucket хранит slot)
- [ ] free-list / live_count
- [ ] `rebuild_runtime()` из slots
- [ ] API: slot_find / slot_meta / slot_read / slot_allocate / slot_update / slot_free
- [ ] iter / count / clear
- [ ] `check_invariants()`
- [ ] randomized model test vs эталонная модель

Часть разработки, а не «потом»:

```text
host:
    check_invariants()      // проверяет инварианты §2.7 архитектуры
    randomized model test   // после каждой случайной операции сверка с reference-моделью
    sanitizers / valgrind

esp32-p4:
    тот же API + allocator/lock integration
    emulator, затем реальный контроллер — без изменений кода
```

## 5. Решения (decision log)

| Дата | Решение | Причина |
|---|---|---|
| 2026-09-30 | Заведён журнал реализации | фиксировать «делаем / собираемся делать» |
| 2026-09-30 | Scope v1 — RAM-only Table Store | сначала доказать корректность slots, persistence/Ring позже |
| 2026-09-30 | Сразу два контура тестирования: host + ESP32-P4 test app | host — алгоритм/скорость/sanitizers; P4 — интеграция embedded-кода |
| 2026-09-30 | Один core, `port/{host,espidf}`; без отдельного host API | не раздваивать API, различать только port |
| 2026-09-30 | Не вводить третью среду (IDF `target=linux`) в v1 | экспериментально, ограниченная поддержка компонентов |
| 2026-09-30 | `check_invariants()` и randomized model test — часть v1 | даёт больше уверенности, чем ручные unit-тесты |
| 2026-09-30 | Архитектура заморожена до результатов v1 | не проектировать предположения |

## 6. Хронология

### 2026-09-30

- Архитектура `MICRO_DB.md` доведена до Ревизии 4 и зафиксирована.
- Заведён журнал; определён scope первой ревизии.

Следующий шаг:

1. создать структуру компонента `mstore`
2. сразу создать два test target: host и ESP32-P4
3. реализовать минимальный platform layer для обоих
4. начать slot core

## 7. Открытые вопросы (из архитектуры)

1. Аллокация: core выделяет сам или принимает готовый буфер.
2. Lock и `iter`: колбэк не мутирует ту же таблицу (зафиксировать контракт).
3. Runtime index: hash-стратегия, deletion policy, load factor — при реализации.
4. Persistence: RAM-only или RAM+Flash; CRC/atomicity/wear/layout — позже.
5. `iter` vs `list`: `iter` в mstore, `list(filter)` в Domain.
