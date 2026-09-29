# micro_db v2 — Architecture and Development Draft

> Черновик развития `micro_db` из текущего внутреннего компонента в самостоятельный reusable ESP-IDF storage engine.

## 1. Цель

`micro_db` — это **fixed-capacity embedded storage primitives**, а не только
record/state store. Предсказуемая память и отсутствие лишних аллокаций важнее удобства
динамических контейнеров.

Внутри — **два независимых storage primitive**, каждый со своей семантикой данных:

```text
micro_db
├── Table Store
│     keyed mutable records
│     get / upsert / remove / list
│
└── Ring Store
      ordered bounded records
      append / read by seq or ref / oldest / newest / overwrite-oldest
```

Table отвечает на вопрос «какое текущее значение записи с этим key». Ring отвечает на
вопрос «какие записи последовательно пришли за последнее окно времени». Это разные
модели данных. Journal и transient payload — не entities и **не** проходят через generic
entity CRUD; они реализуются на Ring Store.

Общие свойства обоих primitive:

- фиксированные слоты, память выделяется заранее на всю capacity;
- отсутствие лишних аллокаций;
- быстрый доступ к горячим данным в RAM;
- recovery и integrity после reboot/power loss (Table; для Ring — позже, см. roadmap).

Дополнительно Table Store даёт дешёвую проверку изменений без чтения payload
(`handle/check`). Zero-copy read (borrow) — **не** основная цель v2, только
оптимизация по замерам (см. §9, §19).

Компонент не должен зависеть от Home Automation.

Он не знает про:

```text
Zigbee
Domain
Journal
WebSocket
LVGL
Automation
UI
Devices
```

## 2. Table Store: что уже есть в v1

Текущая реализация Table уже имеет правильный фундамент:

```text
table
├── records[]
├── slot_used[]
├── free_slots[]
├── primary index: key → slot
├── schema
└── mutex
```

RAM заранее выделяется на всю capacity:

```c
records = calloc(max_records, record_size);
```

Запись физически лежит по адресу:

```c
records + slot * record_size
```

Существующий `upsert()` обновляет payload существующего слота на месте.

При `remove()`:

```text
payload zeroed
slot_used = 0
slot returned to free list
```

Flash persistence также уже организован по слотам:

```text
Flash table
├── header
├── slot_used[]
├── slot_crc[]
└── records[]
```

И может писать конкретный измененный slot.

То есть v1 уже является slot-based storage engine. Главный недостающий слой — полноценная
metadata/handle модель наружу (borrow — опционально, только по замерам).

## 3. Проблема текущего read API

Сейчас публичное чтение работает через copy-out:

```c
micro_db_table_get(table, key, out_record);
```

Внутри:

```text
find key
  ↓
resolve slot
  ↓
memcpy(record → out_record)
```

Это безопасно и должно остаться как Simple API.

Но slot-based архитектура позволяет иметь более быстрый путь без постоянного lookup и payload copy.

## 4. Slot metadata v2

Для каждого slot вводится отдельная metadata:

```c
typedef struct {
    uint32_t generation;
    uint32_t version;
    uint8_t used;
} micro_db_slot_meta_t;
```

### used

```text
0 = free
1 = occupied
```

### generation

Определяет lifetime владельца слота.

```text
slot 17, generation 4 → record A
remove A
slot 17, generation 5 → record B
```

`generation` защищает от stale handle после повторного использования слота.

### version

Определяет ревизию payload текущего владельца.

```text
slot 17, generation 4, version 10
update
slot 17, generation 4, version 11
```

Коротко:

```text
generation = это всё ещё та же запись?
version    = изменились ли данные этой записи?
```

## 5. Handle

Клиент может один раз разрешить key и дальше хранить lightweight handle:

```c
typedef struct {
    uint32_t slot;
    uint32_t generation;
    uint32_t version;
} micro_db_handle_t;
```

Handle не содержит payload и не является raw pointer.

## 6. Resolve

Первичный slow-path:

```c
esp_err_t micro_db_table_resolve(
    const micro_db_table_t *table,
    const void *key,
    micro_db_handle_t *out_handle);
```

Путь:

```text
key
 ↓
primary index
 ↓
slot
 ↓
slot metadata
 ↓
handle
```

После этого consumer не обязан выполнять hash lookup на каждом цикле.

## 7. Cheap check

Одна из главных возможностей v2 — проверка metadata без чтения payload.

Например:

```c
esp_err_t micro_db_table_check_handle(
    const micro_db_table_t *table,
    const micro_db_handle_t *handle,
    micro_db_record_status_t *out_status,
    micro_db_slot_meta_t *out_meta);
```

Семантика:

```text
check(handle)
   │
   ├─ slot free              → REMOVED
   ├─ generation mismatch    → STALE
   ├─ version same           → CURRENT
   └─ version changed        → CHANGED
```

Hot-path большинства polling consumers:

```text
check metadata
   ↓
CURRENT
   ↓
return
```

Payload вообще не читается.

## 8. Snapshot API остается

Простой API не нужно ломать:

```c
micro_db_table_get(...)
micro_db_table_get_by_slot(...)
```

Он остается:

```text
safe
simple
copy-out
```

При stack buffer это zero-allocation, хотя и не zero-copy.

## 9. Zero-copy borrowed read (optional)

> Это **не** основная цель v2. Базовый fast-path — `resolve → check → read(copy-out)`.
> Borrow добавляется только после профилирования конкретного workload (см. §19).

Для измеренного hot-path может добавляться borrowed access (держит table-lock на время
чтения, поэтому только один borrower за раз):

```c
esp_err_t micro_db_table_acquire(
    const micro_db_table_t *table,
    const micro_db_handle_t *handle,
    const void **out_record,
    uint32_t *out_version);

void micro_db_table_release(
    const micro_db_table_t *table);
```

Семантика:

```text
acquire
  ↓
lock
  ↓
validate slot + generation
  ↓
return const pointer into records[]
  ↓
consumer reads directly
  ↓
release
  ↓
unlock
```

Raw pointer валиден только внутри borrow scope.

Клиент хранит handle, а не pointer.

## 10. Пример hot-path consumer

Для Display widget:

```text
Widget
├── handle.slot
├── handle.generation
└── last_version

render tick
   ↓
check(handle)
   │
   ├─ CURRENT → return
   ├─ REMOVED → clear widget
   ├─ STALE   → resolve key again
   └─ CHANGED
          ↓
       read copy-out (get_by_slot)        [if measured: acquire/release]
          ↓
       update LVGL
```

Обычный цикл (copy-out) не требует:

- heap allocation;
- key lookup;
- отдельного application-state cache в UI.

Borrow (`acquire/release`) убирает ещё и memcpy, но держит table-lock на время чтения —
поэтому применяется только по замерам.

## 11. RAM + Flash model (Table Store)

RAM остается рабочим представлением таблицы.

Flash persistence отражает slot layout.

Целевая модель:

```text
RAM
├── slot_meta[]
└── records[]

Flash
├── table header
├── persisted slot metadata
├── per-slot CRC
└── records[]
```

При upsert конкретного slot:

```text
update RAM slot
   ↓
update metadata
   ↓
persist slot payload if needed
   ↓
persist slot metadata
```

Не требуется переписывать всю таблицу.

## 12. Persistence policy (Table Store)

Политика остается декларативной через schema/backing:

```text
RAM only
RAM + Flash
```

Текущая реализация фактически требует RAM для runtime table; это нормально для основной модели v2.

Flash рассматривается как persistence backing, а не как random-access runtime payload backend.

## 13. Recovery и integrity (Table Store)

Persistent table должна проверяться при старте.

Минимально сохраняются:

- layout magic/version;
- record_size;
- capacity;
- slot metadata;
- per-slot checksum;
- live count или восстанавливаемая информация.

Boot:

```text
load flash image
   ↓
validate schema/layout
   ↓
validate slot metadata
   ↓
validate CRC
   ↓
restore RAM records/meta
   ↓
rebuild primary index
   ↓
rebuild free list
```

Runtime index хранить во Flash необязательно.

## 14. Public API levels

У v2 должно быть три уровня сложности.

### Simple

```text
get(key)
upsert(record)
remove(key)
```

### Metadata-aware

```text
resolve(key)
check(handle)
get_by_slot(slot)
```

### Zero-copy (optional, только по замерам)

```text
acquire(handle)
release()
```

Обычный клиент не обязан знать про handles и borrow.

## 15. Iteration

Текущие iterator APIs полезны и должны сохраниться:

```text
iter(record)
iter_slots(slot, record)
```

В v2 можно дополнительно рассмотреть metadata iterator:

```text
iter_meta(slot, meta)
```

для дешевого обхода без payload read.

## 16. Ring Store

Отдельный primitive, **не связанный** с primary-key CRUD Table Store. Ordered bounded
records с монотонным seq.

Ring Store — **low-level storage primitive**, не application-facing API. Приложение
работает с ним через фасад слоя выше, а не через `micro_db` ring-функции напрямую.

```text
records[capacity]
write_cursor
count
monotonic seq
slot generation или эквивалентная stale-защита
lock
```

Ring отвечает на вопрос «какие записи последовательно пришли за последнее окно времени».
Он **не** отвечает на вопрос «какое текущее значение записи с этим key».

### Семантика

```text
append new record
→ получает monotonic seq/ref
→ если capacity не заполнена: занимает следующий slot
→ если заполнена: overwrite oldest
→ старый seq/ref становится stale
```

Ring **не имеет**:

- hash index;
- primary key;
- free-list semantics обычной table;
- remove произвольной записи;
- generic entity CRUD.

Это append/overwrite структура.

### Identity: seq

Желательно использовать `seq` как основную identity ring record, а не `{slot,generation}`,
потому что это естественно для Journal:

```c
typedef uint64_t micro_db_ring_seq_t;
```

`get_by_seq(seq)`:

```text
seq внутри текущего retention window → вернуть запись
seq уже вытеснен                  → STALE / NOT_FOUND
```

Внутренне slot можно вычислять по seq/cursor. Если для реализации удобнее
`{slot,generation}`, это остаётся внутренней деталью; public API заранее не усложняем.

### Conceptual API

```c
micro_db_ring_append(...)
micro_db_ring_get(...)
micro_db_ring_get_by_seq(...)
micro_db_ring_oldest_seq(...)
micro_db_ring_newest_seq(...)
micro_db_ring_contains(...)
```

Точные имена обсуждаемы; фиксируется семантика выше.

### Persistence

Ring Store — отдельный primitive и **не** зависит от zero-copy borrow API Table.
Persistence Ring (flash/SD) в v2 пока не проектируется: на текущем этапе это
fixed-capacity RAM структура.

## 17. Что micro_db не делает

Внутри не должно появляться:

```text
Domain events
subscriptions приложения
callbacks UI
WebSocket notifications
Zigbee mapping
Automation triggers
entity-specific CRUD
application commands
```

`micro_db` сообщает вызывающему коду только результат операции:

```text
OK / error
changed?
inserted?
removed?
metadata
```

Любая event-based логика строится слоем выше.

Ни Table Store, ни Ring Store не знают про Journal, Archive, Domain или Zigbee semantics —
оба остаются storage mechanics.

## 18. Роль в Home Automation v2

Home Automation services используют `micro_db` только через Domain facade:

```text
Home Automation services (Zigbee / Web / Automation / Display)
      ↓
Domain facade
      ↓
micro_db internals
├── Table Store   → Entity Store
├── Ring Store    → Journal
└── Ring Store    → Transient Payload
```

Domain владеет смыслом сущностей и событий и предоставляет сервисам упрощённый API
(entity CRUD, commands, payload put/get, subscription/trigger). Сервисы **не** используют
`micro_db` Ring/Table API напрямую и не видят `micro_db` seq / slot / generation типы.

Доменные identity (`domain_event_id_t`, `domain_payload_ref_t`) принадлежат Domain API.
Domain отдаёт наружу свои opaque id-типы; реализация может напрямую кодировать `micro_db`
ring seq, отдельной таблицы сопоставления не требуется.

`micro_db` владеет только storage mechanics. Его Ring Store — low-level storage primitive,
не application-facing API.

Display использует metadata/handle API только через Domain read facade, не создавая
отдельную копию state. Прямой линк к `micro_db` минуя Domain не вводится.

## 19. План развития от текущей версии

### Phase 1 — tests/fix текущей Table v1

- добавить unit tests на slot allocation/reuse;
- проверить remove действительно zeroes payload;
- тестировать key → slot consistency;
- тестировать flash slot persistence/recovery;
- зафиксировать существующее поведение API.

### Phase 2 — Table slot metadata (generation/version)

- заменить/расширить `slot_used[]` на `slot_meta[]`;
- добавить `generation`;
- добавить `version`;
- определить wraparound semantics;
- обновить rebuild/recovery.

### Phase 3 — Table handle/check/read

- `resolve(key) -> handle`;
- `check(handle)`;
- `get_by_slot` (copy-out);
- stale/remove/change tests;
- slot reuse tests.

### Phase 4 — Ring Store: basic RAM

- `records[capacity]` + `write_cursor` + `count` + monotonic seq;
- `append` с overwrite-oldest;
- `oldest_seq` / `newest_seq`;
- базовые tests на заполнение и перезапись.

### Phase 5 — Ring seq/ref + stale semantics + tests

- `seq` как identity;
- `get_by_seq` → STALE/NOT_FOUND для вытесненных;
- `contains(seq)`;
- stale/overwrite tests.

### Phase 6 — optional zero-copy for Table (only if measured)

- `acquire(handle)` / `release()`;
- чётко документировать pointer lifetime;
- tests на concurrent remove/upsert;
- убедиться, что lock scope минимален;
- **только по результатам профилирования**.

### Phase 7 — persistence work

- Table flash layout v2: generation/version metadata, layout migration policy,
  checksum metadata/payload, corrupted slot recovery tests, power-loss scenarios;
- Ring persistence (flash/SD) — отдельная тема, позже, только под реальную потребность.

### Phase 8 — performance validation

Измерить:

- key lookup + get;
- check(handle);
- acquire(handle) (если внедрён);
- RAM usage per slot / per ring;
- Flash write amplification;
- Display polling workload.

Оптимизации делать только после измерений.

### Phase 9 — отделение и публикация

- вынести из Home Automation в самостоятельный repository/component;
- структура:

```text
micro_db/
├── CMakeLists.txt
├── idf_component.yml
├── include/micro_db/
├── src/
├── test/
├── examples/
├── README.md
└── LICENSE
```

- semantic versioning, API documentation;
- examples: RAM-only table, persistent table, handle/check consumer, ring;
- CI на поддерживаемых ESP-IDF versions;
- публикация в ESP Component Registry.

## 20. Design rules

Table Store:

1. Fixed memory layout важнее удобства динамических контейнеров.
2. Payload хранится в одном canonical location.
3. Metadata отделена от payload.
4. Самый частый путь должен быть самым дешевым.
5. `check()` не должен читать payload.
6. Raw pointer никогда не является долгоживущим public identity.
7. Handle должен переживать updates, но обнаруживать remove/reuse.
8. `get()` остается простым и безопасным даже после появления fast-path.

Ring Store:

9. Ring — ordered append/overwrite, без key/hash/free-list/remove-by-key.
10. `seq` — долгоживущая identity ring record; slot — внутренняя деталь.
11. Вытесненный seq даёт STALE/NOT_FOUND, а не ошибку жизненного цикла.

Общие:

12. Flash persistence не должна проникать в application code.
13. Component остается полностью domain-agnostic.

## 21. Ключевая формула v2

Table:

```text
key
 ↓ resolve once
handle {slot, generation, version}
 ↓
check metadata many times
 ↓
unchanged → return
changed   → read copy-out (get_by_slot)      [optional: acquire → use → release]
```

Ring:

```text
append record
 ↓ gets monotonic seq
 ↓
capacity full → overwrite oldest (старый seq → STALE)
 ↓
get_by_seq(seq) внутри окна → record
```

Общая формула для Home Automation:

```text
micro_db Table
    → mutable current state
    → Entity Store

micro_db Ring
    → bounded ordered records
    → Journal
    → Transient Payload
```

Цель `micro_db v2` — дать embedded-коду предсказуемое хранение и очень дешевый доступ к
горячему состоянию (через Table) и к упорядоченному скользящему окну записей (через Ring),
не навязывая сложность тем consumers, которым достаточно обычного `get()`.
