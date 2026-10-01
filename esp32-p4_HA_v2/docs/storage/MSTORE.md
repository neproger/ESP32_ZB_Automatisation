# mstore — Architecture

> **Ревизия 4.** Описание структуры и намерений компонента.
> Это **не план реализации**: API и типы уточняются в коде.

## 1. Обзор

`mstore` — набор **fixed-capacity storage primitives** для embedded. Цель —
предсказуемая память и отсутствие лишних аллокаций, а не удобство динамических
контейнеров.

Намерения компонента:

- фиксированные слоты, память выделяется заранее на всю capacity;
- быстрый доступ к горячим данным в RAM;
- дешёвая проверка изменения без чтения payload;
- recovery/integrity после reboot;
- опциональная flash-персистентность — слой поверх RAM-модели;
- полная независимость от предметной области;
- platform-independent C-core, собираемый и тестируемый на host.

Внутри — **два независимых primitive**, отвечающих на разные вопросы:

```text
Table Store   — «какое текущее значение записи с этим key»
Ring Store    — «какие записи последовательно пришли за последнее окно»
```

Это разные модели данных, и они не смешиваются.

## 2. Table Store

**Роль:** storage primitive над **фиксированным массивом slots**. Это не мини-БД:
основной контракт — slot-first, а не CRUD с key в центре.

Логическая структура:

```text
slot
├── meta
│   ├── used
│   ├── generation
│   └── version
├── key
└── payload
```

```text
meta    — служебное состояние slot, принадлежит mstore
key     — логическая identity записи, часть canonical slot state
payload — пользовательские данные caller'а
slot    — физическое место в table
```

`key` **не является частью payload** и не извлекается из него через callback. По key
находится slot, после чего по slot читаются `meta / key / payload`.

### 2.1. Термины

```text
table   — fixed-capacity массив slots
slot    — физическое место записи в table
meta    — служебное состояние slot {used, generation, version}
key     — логическая identity записи
payload — пользовательские данные записи
```

Запись физически — `meta + key + payload`; наружу части отдаются раздельно.

### 2.2. Физическая модель

- RAM выделяется заранее на всю capacity; таблица — массив слотов;
- `slot = meta + key + payload`;
- фиксированный stride: `slot_size = sizeof(meta) + key_size + payload_size`;
- слоты переиспользуются;
- runtime — RAM; flash — backing персистентности, не меняющий модель.

```text
[meta][key][payload][meta][key][payload]...   // фиксированный stride

slot_addr = base + slot * slot_size
meta      = slot_addr
key       = slot_addr + sizeof(meta)
payload   = slot_addr + sizeof(meta) + key_size
```

### 2.3. Ownership

```text
meta    → полностью управляется mstore
key     → хранится mstore, задаётся caller'ом при allocate, далее неизменен
payload → задаётся caller'ом
```

Caller при `slot_allocate` передаёт `key` и `payload`; `used / generation / version` он
не задаёт и не меняет никогда. Смена identity — только `slot_free` + `slot_allocate`.

### 2.4. slot, key и identity

- долговечная логическая identity — `key`;
- физическое место — `slot`; он переиспользуется и **не** является долговечной identity;
- закешированная ссылка consumer'а — `slot + generation`;
- key в пределах incarnation неизменен: `slot_update` его не трогает.

### 2.5. Canonical state и derived runtime state

`meta.used` определяет семантическую валидность `key` и `payload`:

```text
meta.used == 1:
    meta + key + payload — действующая запись

meta.used == 0:
    действующей записи нет; bytes key/payload не имеют семантики и игнорируются
```

```text
canonical model = логический fixed-capacity массив slots
    slots[].meta
    slots[].key      // валиден только при meta.used == 1
    slots[].payload  // валиден только при meta.used == 1

physical residence (где физически лежат canonical bytes):
    определяется storage backend (RAM / FLASH / RAM+FLASH), см. §2.11

derived runtime acceleration:
    index        (key → slot)
    free-list
    live_count
```

Index, free-list и live_count **не** являются источником истины: они полностью
восстанавливаются из canonical slots (через storage backend).

```text
rebuild_runtime():
    for each slot:
        if meta.used:
            index_insert(slot)      // bucket хранит slot; key берётся из slot.key
    live_count = число used
    free-list  = слоты с used == 0
```

Runtime state можно выбросить и восстановить в любой момент: после init/recovery, при
подозрении на нарушение инварианта, в тестах/диагностике.

| Понятие | Значение |
|---|---|
| `key` | долговечная логическая identity |
| `slot` | физическое место |
| `generation` | incarnation физического slot |
| `version` | редакция payload внутри incarnation; дешёвая проверка изменения |
| `used` | существуют ли сейчас key/payload |
| `index` | ускоритель `key → slot` |
| `free-list` | ускоритель allocate |
| `live_count` | кеш количества |
| `slot + generation` | безопасная кешированная ссылка |

### 2.6. generation / version

```text
slot       — где находится запись
generation — какая логическая жизнь сейчас занимает этот physical slot
version    — какая редакция payload внутри текущей generation
```

Lifecycle:

```text
allocate:
    used:       0 -> 1
    generation: increment
    version:    initial value

update:
    used:       remains 1
    generation: unchanged
    version:    increment only if payload реально изменился

free:
    used:       1 -> 0
    generation: unchanged

reuse того же physical slot:
    generation: increment
    version:    reset to initial value
```

`generation` защищает физический адрес от ABA/reuse: закешированный `slot + generation`
перестаёт обозначать старую логическую запись, как только слот переиспользован.
`version` относится к текущему incarnation и меняется только при реальном изменении
payload (через `payload_equals`).

Начальное значение (0/1) — вопрос реализации. Wraparound **не допускается**:

```text
allocate:
    slot с generation == UINT32_MAX не переиспользуется
    (все свободные slots исчерпаны -> SYS_CODE_OVERFLOW)

update:
    реальное изменение при version == UINT32_MAX -> SYS_CODE_OVERFLOW
```

Так исключается ABA по generation и неоднозначная ревизия по version.

Пример жизненного цикла одного физического slot'а:

```text
// свободный slot
slot 5:
    meta.used       = 0
    meta.generation = 12
    meta.version    = 0

slot_allocate(key=42, payload={state=OFF})
slot 5:
    meta.used       = 1
    meta.generation = 13        // increment при allocate
    meta.version    = 1
    key             = 42
    payload         = {state=OFF}

slot_update(5, {state=ON})
slot 5:
    meta.used       = 1
    meta.generation = 13        // та же логическая запись
    meta.version    = 2         // данные изменились
    key             = 42
    payload         = {state=ON}

slot_free(5)
slot 5:
    meta.used       = 0
    meta.generation = 13        // free не меняет generation
    meta.version    = 2         // остаётся от прошлого incarnation
    key/payload     = не имеют семантики (used == 0)

// позже тот же физический slot занимает другая запись
slot_allocate(key=77, payload={state=OFF})
slot 5:
    meta.used       = 1
    meta.generation = 14        // новый владелец slot
    meta.version    = 1         // reset to initial value
    key             = 77
    payload         = {state=OFF}
```

### 2.7. Инварианты Table Store

```text
live_count == количество slots с meta.used == 1
каждый used slot имеет ровно один уникальный key
каждый entry runtime index указывает на used slot
каждый used slot представлен в runtime index
free-list содержит только slots с used == 0
slot не может одновременно быть live и free
rebuild_runtime(slots) полностью восстанавливает index / free-list / live_count
```

Уникальность key поддерживает `slot_allocate`:

- key проверяется по index;
- если key уже существует → `ALREADY_EXISTS`, дубликат не создаётся;
- если свободных slots нет → `NO_SPACE` (capacity exhausted);
- дубликаты key внутри одной table запрещены.

### 2.8. API и семантика

```text
slot_find(table, key, *slot)

slot_meta(table, slot, *meta)

slot_read(table, slot, *meta, *key, *payload)

slot_allocate(table, key, payload, *slot, *generation)

slot_update(table, slot, expected_generation, payload, *changed)

slot_free(table, slot, expected_generation)

iter / count / clear / rebuild_runtime
```

| Возможность | Семантика |
|---|---|
| `slot_find` | key → slot через runtime index |
| `slot_meta` | дешёвый `meta` по slot; payload не читается |
| `slot_read` | атомарный snapshot `meta + key + payload` |
| `slot_allocate` | занять свободный slot под `(key, payload)`, вернуть `slot + generation` |
| `slot_update` | обновить payload при совпадении `expected_generation`; вернуть `changed` |
| `slot_free` | освободить slot при совпадении `expected_generation` |
| `iter / count / clear` | обход, размер, очистка |
| `rebuild_runtime` | перестроить index / free-list / live_count из slots |

Таблица не выставляет CRUD с key в центре: `upsert / get / remove` вне ядра Table Store.

**Stale-slot.** Consumer кеширует `slot + generation`. Мутирующие операции принимают
`expected_generation`: если `meta.used == 0` или generation не совпадает — операция
возвращает `STALE` и **не меняет** slot. Отдельного handle-объекта нет; `slot + generation`
остаётся данными consumer'а.

**Результат операций.** `NOT_FOUND` относится к логическому поиску по key, `STALE` — к
протухшей физической ссылке:

```text
slot_find(key):    key отсутствует            → NOT_FOUND
slot_update/free:  used == 0                  → STALE
                   generation != expected     → STALE
slot_update:       payload изменился          → *changed = true
                   payload тот же             → *changed = false, version не растёт
```

**Атомарность чтения.** `slot_meta` и `slot_read` — разные вызовы, между ними поколение
slot'а может смениться. Поэтому `slot_read` под одним lock возвращает snapshot минимум
`meta + key + payload`; по возвращённой `meta` consumer определяет generation прочитанной
записи. `slot_meta` не гарантирует, что следующий `slot_read` увидит ту же generation — это
определяется по meta, которую вернул сам `slot_read`.

**Hot path:**

```text
slot_find(key) → slot
slot_meta(slot)
    generation/version не изменились → payload не читаем
    изменились                       → slot_read(slot) snapshot
```

**Zero-copy borrow** (`acquire/release`) — не основная цель: опциональная оптимизация,
добавляется только по результатам замеров.

### 2.9. slot_find и runtime index

Штатный `slot_find(key)` идёт через runtime index, а **не** через линейный scan:

```text
key → runtime index → slot → meta / key / payload
```

```text
slot_find(key):
    lookup in index          // успешный lookup означает slot.key == key
    validate slot.used == 1
    return slot
```

Успешный index lookup означает совпадение canonical `slot.key`; `used` slot также
проверяется. Отдельная повторная проверка key после lookup не нужна — probing и так
сравнивает ключ, — но контракт фиксирует: найденный slot обязан быть `used` и иметь
запрошенный key.

Концептуально index хранит `key → slot`, но key **не дублируется** внутри index без
необходимости: bucket хранит slot, а сравнение key выполняется с canonical `slot.key`.
Конкретный алгоритм hash table (linear probing / backward-shift deletion и т.п.) —
кандидат реализации, а не архитектурный контракт.

Линейный проход по slots — только служебный механизм:

- построение runtime state при init/recovery;
- `rebuild_runtime()`;
- проверка инвариантов;
- тесты/диагностика.

Он не является штатным API-path.

```text
slots      — canonical storage
index      — ускорение slot_find
free-list  — ускорение allocate
live_count — кэшированная статистика
```

### 2.10. Schema

Key в Table Store — **непрозрачная byte sequence фиксированной длины** `key_size`.
Равенство определяется побайтовым сравнением всех `key_size` bytes, hash считается по
тем же bytes. Поэтому callback'и `key_of / key_equals` не нужны: key хранится mstore.
Schema задаёт размеры:

```c
typedef struct {
    size_t capacity;
    size_t key_size;       // key: opaque bytes фиксированной длины
    size_t payload_size;

    bool (*payload_equals)(const void *a, const void *b);  // опц.; иначе memcmp

    mstore_backing_t backing;  // RAM / FLASH / RAM|FLASH (см. §2.11.1)
    const char *persist_key;   // stable persistent identity; нужен для FLASH
} mstore_table_schema_t;
```

`payload_equals` нужен только чтобы решить, вырос ли `version`. `backing` задаёт
storage policy один раз при `init()`; после этого caller не знает, где лежат данные.

### 2.11. Table Engine и storage backend

Table Store разделён на два уровня:

```text
Table Engine     — ЧТО означает операция
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

Table Engine владеет **всей** семантикой: key identity, slot lifecycle, generation,
version, expected_generation, `ALREADY_EXISTS / NOT_FOUND / STALE / NO_SPACE`,
`changed`, index, free-list, live_count, iter, count, clear, rebuild, invariants.

Storage backend не знает, что означает key и почему изменился generation/version. Он
только хранит/читает canonical slot state (`meta + key + payload`) и умеет sync/commit.

Публичный `mstore_table_*` API **не зависит** от backend. Caller один раз задаёт
storage policy в schema и после `init()` не знает, где лежат данные. Table Engine не
содержит ветвлений `if RAM / if FLASH / if RAM_FLASH`.

#### 2.11.1. Storage modes

Backing задаётся bitmask (композиция backing'ов, а не отдельный enum из трёх значений):

```c
typedef enum {
    MSTORE_BACKING_NONE  = 0,
    MSTORE_BACKING_RAM   = 1 << 0,
    MSTORE_BACKING_FLASH = 1 << 1,
} mstore_backing_t;
```

```text
RAM_ONLY   = RAM
FLASH_ONLY = FLASH
RAM_FLASH  = RAM | FLASH
```

- **RAM** — canonical slot bytes в RAM; index/free-list/live_count — derived.
- **FLASH** — canonical slot bytes во flash; в RAM только derived runtime structures и
  небольшой scratch. Полный массив payload в RAM не требуется.
- **RAM+FLASH** — composite backend: RAM — working/hot storage, FLASH — durable backing.
  Именно composite владеет load on init, write/commit policy, recovery, rollback и
  consistency policy. Отдельной реализации Table Store под этот режим нет.

#### 2.11.2. Internal storage contract (private)

Концептуально backend обязан уметь:

```text
open / init
close

read_meta(slot)          // hot path; payload не читается
read_key(slot)           // для index probing
read_slot(slot)          // snapshot meta + key + payload

write_slot(slot, state)
write_meta(slot, meta)
clear_slot(slot)
clear_all()

sync / commit            // для FLASH / RAM+FLASH
```

Точные имена, гранулярность и набор операций определяются по реализации. Interface
**internal**: наружу существует только `mstore_table_*`; имён `mstore_ram_*` /
`mstore_flash_*` в публичном API нет.

#### 2.11.3. Что переносится из micro_db v1

```text
KEEP:
    backing задаётся в schema;
    stable persist_key как persistent identity таблицы;
    caller не знает backend;
    load/recover внутри mstore;
    derived runtime state rebuild после load;
    per-table persistent identity.

REWORK / DROP:
    RAM обязателен, FLASH-only не поддерживается;
    records + slot_used как отдельная старая модель;
    read-modify-erase-write 4K сектора на каждую мелкую мутацию;
    глобальный flash singleton, MAX_TABLES, persist_key[16];
    FNV-checksum как единственная integrity-защита.
```

Старый flash backend нельзя переносить напрямую.

#### 2.11.4. Persistence: порядок и формат

- Сначала host-версия flash/NOR **simulator**, а не сразу `esp_partition`. Simulator
  моделирует ограничения flash: erased byte `0xFF`, write only `1 → 0`, `0 → 1` только
  через erase, erase block size, bounded region; плюс fault injection: fail after N
  written bytes, fail during erase, fail before/after commit, corrupt bytes, reopen
  after simulated reboot.
- Durable format проектируется **отдельно** и пока не фиксируется. Append-only /
  journaled layout (record header, slot identity, generation/version, used, key,
  payload, CRC, commit marker/sequence) — гипотеза, а не решение. Выбор по критериям:
  power-loss safety, atomic observable state, recovery, bounded RAM, erase/write
  amplification, compaction, corruption detection.
- ESP-IDF `esp_partition` backend реализован (adapter + partition label
  `CONFIG_MSTORE_FLASH_PARTITION_LABEL`); hardware verification на P4 — отдельный шаг.
- Persistence остаётся внутри mstore и не меняет семантику Table Store.

#### 2.11.5. Error semantics

Публичная семантика одинакова для всех backing modes:
`NOT_FOUND / STALE / ALREADY_EXISTS / NO_SPACE / INVALID_ARG / INVALID_STATE /
INVALID_SIZE / NO_MEM`. Caller не видит RAM/flash-специфику. Новый нейтральный
`sys_error_t` для IO/corruption вводится только с отдельной архитектурной фиксацией.

#### 2.11.6. Тесты поверх storage modes

Один behavioral suite и один reference model поверх всех режимов:

```text
run_table_suite(RAM)
run_table_suite(FLASH)
run_table_suite(RAM | FLASH)
```

Для caller observable behavior совпадает. Для persistent режимов дополнительно:
reboot after every mutation, power-loss на границах записи, corrupt header/slot,
partial write/erase, compaction interruption, schema mismatch, `persist_key`
mismatch/collision. После recovery инварианты обязаны выполняться.

Ring Store в persistence refactor **не втягивается**, пока storage architecture не
доказана на Table Store (см. §3).

## 3. Ring Store

**Роль:** ordered bounded records — скользящее окно последовательных записей.

**Модель:**

- `records[capacity]` + write cursor + count;
- **monotonic seq** как долгоживущая identity записи;
- при заполнении вытесняется самая старая запись (overwrite-oldest);
- slot вычисляется по seq/cursor и остаётся внутренней деталью.

**За что отвечает:**

| Возможность | Смысл |
|---|---|
| `append` | добавить запись, получить seq |
| `get_by_seq` | чтение записи в пределах окна; вытесненный seq → STALE/NOT_FOUND |
| `oldest / newest` | границы текущего окна |
| `contains` | есть ли seq в окне |

**Чего у Ring нет:** hash-индекса, primary key, free-list, удаления произвольной записи,
generic entity CRUD. Это чистая append/overwrite структура.

**Persistence:** на текущем этапе Ring — fixed-capacity RAM; персистентность — отдельная
тема, не проектируется сейчас.

## 4. Чего mstore не делает

- Не знает про: Domain, Journal, Zigbee, Automation, UI, WebSocket, devices.
- Не содержит событий, подписок, callback'ов, маппинга и trigger-логики.
- Не заводит entity-specific CRUD и application-команды.

Любому вызывающему mstore сообщает только результат операции: `OK / error`, `metadata`
и результат мутации (`changed`). Вся event-based логика строится слоем выше.

Оба primitive остаются **storage mechanics** и ничего не знают про смысл данных.

## 5. Роль в Home Automation

```text
Home Automation services (Zigbee / Web / Automation / Display)
        ↓
Domain facade
        ↓
mstore
├── Table Store   → Entity Store
├── Ring Store    → Journal
└── Ring Store    → Transient Payload
```

- Сервисы работают только через Domain facade и **не** видят mstore API/типы
  (seq, slot, generation, ring-функции наружу не выходят).
- Доменные identity (`event_id`, `payload_ref`) принадлежат Domain; реализация может
  кодировать mstore seq напрямую — отдельной таблицы сопоставления не требуется.
- mstore владеет только storage mechanics; Domain владеет смыслом сущностей и событий.

## 6. Контракт (предложение)

Общие соглашения:

- возвращаемое значение — `sys_error_t` из общего пространства кодов (`OK / NOT_FOUND /
  ALREADY_EXISTS / STALE / INVALID_ARG / NO_MEM / NO_SPACE / INVALID_STATE /
  INVALID_SIZE / INVARIANT_FAILED / IO / CORRUPT / OVERFLOW`); mstore ставит
  `layer = MSTORE` и ни одну чужую ошибку не перекодирует (`ERRORS.md`);
- экземпляр table/ring — caller-owned структура; жизненный цикл `init / deinit`;
- capacity фиксирована; память выделяется core'ом через platform allocator;
- у экземпляра один внутренний lock (platform lock); все операции thread-safe
  относительно него.

Table Store API и семантика — §2.8, schema — §2.10.

### 6.1. Ring Store

Конфигурация: `record_size`, `capacity`.

```text
init(ring, config) / deinit(ring) / count(ring)
append(ring, record, *seq)
get_by_seq(ring, seq, *record)               // вытесненный seq → STALE / NOT_FOUND
oldest_seq(ring, *seq) / newest_seq(ring, *seq)
contains(ring, seq, *bool)
```

`seq` — `uint64`, монотонный; slot/cursor — внутренняя деталь.

## 7. Намерения (design rules)

1. Fixed memory layout важнее удобства динамических контейнеров.
2. Canonical state — логический массив slots (`meta + key + payload`); физическое
   расположение определяет storage backend (RAM / FLASH / RAM+FLASH); `index /
   free-list / live_count` — производные и восстанавливаемые ускорители.
3. Storage владеет `meta` и `key`; caller владеет только `payload`.
4. Самый частый путь самый дешёвый: `key → slot` один раз, дальше доступ по slot.
5. `slot_meta()` не читает payload.
6. `slot_update()` не меняет key и возвращает `changed`; смена identity — `free + allocate`
   (новая generation).
7. Raw pointer не является долгоживущей identity; долгоживущее — key.
8. Consumer хранит `slot + generation` (и `version`); mstore handles не ведёт.
9. Мутирующие операции принимают `expected_generation` и защищают от stale slot.
10. `slot_read()` атомарно возвращает snapshot `meta + key + payload`.
11. `slot_find` ускоряется index'ом; линейный проход — только rebuild/repair.
12. Table Store не выставляет CRUD с key в центре (`upsert / get / remove` вне ядра).
13. Ring — ordered append/overwrite: без key, hash, free-list и remove-by-key.
14. `seq` — долгоживущая identity Ring-записи; slot — внутренняя деталь.
15. Вытесненный seq даёт STALE/NOT_FOUND, а не ошибку жизненного цикла.
16. Persistence не меняет семантику Table Store и не проникает в application code.
17. Компонент полностью домен-агностичен.
18. Core platform-independent; ESP-IDF — адаптер, а не среда выполнения core.
19. Public Table API не зависит от storage mode; backend задаётся в schema один раз, и
    caller после `init()` не знает, где физически лежат canonical bytes.

## 8. Реализация и платформенная независимость

- Core `mstore` — platform-independent C, собирается и тестируется на host.
- Core зависит только от минимального platform layer:
  - allocator;
  - lock/mutex.
- ESP-IDF (`esp_err_t`, FreeRTOS mutex, partition/flash) — адаптер, а не обязательная
  среда выполнения core.
- В core первой ревизии нет `crc32` и прочей persistence-специфики; checksum появляется
  вместе с persistence layer.

## 9. Открытые вопросы

1. **Аллокация.** Core выделяет сам (сейчас так) или принимает заранее выделенный буфер.
2. **Lock и итерация.** `iter` под lock; колбэк получает `slot/meta/key/payload` и поэтому
   не вызывает API этой же table. Lock не обязан быть рекурсивным: поведение не зависит
   от порта.
3. **Runtime index.** Стратегия зафиксирована (linear probing + backward-shift,
   load <= 0.5); открыто только возможное изменение по результатам benchmark.
4. **Durable format.** Append-only/journaled или иное; compaction, erase/write
   amplification, corruption detection — проектируется на host-simulator'е.
5. **RAM+FLASH commit policy.** write-through vs batched; recovery/rollback — внутри
   composite backend.
6. **Ring persistence.** Нужен ли общий storage abstraction для Ring или
   специализированный layout — решать после стабилизации Table persistence.
7. **`iter` vs `list`.** mstore даёт `iter`; `list(filter)` собирает Domain.
