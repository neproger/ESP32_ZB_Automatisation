# micro_db v2 — Architecture and Development Draft

> Черновик развития `micro_db` из текущего внутреннего компонента в самостоятельный reusable ESP-IDF storage engine.

## 1. Цель

`micro_db` должен стать компактным универсальным record/state store для embedded-систем, где важны:

- предсказуемая память;
- отсутствие лишних аллокаций;
- фиксированные слоты;
- быстрый доступ к горячим данным в RAM;
- optional Flash persistence;
- дешёвая проверка изменений без чтения payload;
- zero-copy read для hot-path;
- recovery и integrity после reboot/power loss.

Компонент не должен зависеть от Home Automation.

Он не знает про:

```text
Zigbee
Domain
WebSocket
LVGL
Automation
UI
Devices
```

## 2. Что уже есть в v1

Текущая реализация уже имеет правильный фундамент:

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

То есть v1 уже является slot-based storage engine. Главный недостающий слой — полноценная metadata/handle/borrow модель наружу.

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

## 9. Zero-copy borrowed read

Для hot-path добавляется borrowed access:

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
       acquire
          ↓
       read payload directly
          ↓
       update LVGL
          ↓
       release
```

Обычный цикл не требует:

- heap allocation;
- payload memcpy;
- key lookup;
- отдельного application-state cache в UI.

## 11. RAM + Flash model

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

## 12. Persistence policy

Политика остается декларативной через schema/backing:

```text
RAM only
RAM + Flash
```

Текущая реализация фактически требует RAM для runtime table; это нормально для основной модели v2.

Flash рассматривается как persistence backing, а не как random-access runtime payload backend.

## 13. Recovery и integrity

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

### Zero-copy

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

## 16. Что micro_db не делает

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

## 17. Роль в Home Automation v2

Home Automation использует `micro_db` через Domain:

```text
Home Automation
      ↓
Domain
      ↓
micro_db
```

Domain владеет смыслом сущностей и событий.

`micro_db` владеет только storage mechanics.

Display сможет использовать metadata/handle/zero-copy API напрямую через подходящий Domain read facade или низкоуровневый read path, не создавая отдельную копию state.

## 18. План развития от текущей версии

### Phase 1 — зафиксировать текущую v1

- добавить unit tests на slot allocation/reuse;
- проверить remove действительно zeroes payload;
- тестировать key → slot consistency;
- тестировать flash slot persistence/recovery;
- зафиксировать существующее поведение API.

### Phase 2 — slot metadata

- заменить/расширить `slot_used[]` на `slot_meta[]`;
- добавить `generation`;
- добавить `version`;
- определить wraparound semantics;
- обновить rebuild/recovery.

### Phase 3 — handle API

- `resolve(key) -> handle`;
- `check(handle)`;
- stale/remove/change tests;
- slot reuse tests.

### Phase 4 — zero-copy borrow

- `acquire(handle)`;
- `release()`;
- четко документировать pointer lifetime;
- tests на concurrent remove/upsert;
- убедиться, что lock scope минимален.

### Phase 5 — Flash layout v2

- сохранить generation/version metadata;
- layout version migration policy;
- checksum metadata/payload;
- corrupted slot recovery tests;
- power-loss scenarios там, где это практично.

### Phase 6 — performance validation

Измерить:

- key lookup + get;
- check(handle);
- acquire(handle);
- RAM usage per slot;
- Flash write amplification;
- Display polling workload.

Оптимизации делать только после измерений.

### Phase 7 — отделение компонента

Вынести из Home Automation в самостоятельный repository/component.

Структура:

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

### Phase 8 — публикация

После стабилизации:

- semantic versioning;
- API documentation;
- example RAM-only table;
- example persistent table;
- example handle/check/zero-copy consumer;
- CI на поддерживаемых ESP-IDF versions;
- публикация в ESP Component Registry.

## 19. Design rules

1. Fixed memory layout важнее удобства динамических контейнеров.
2. Payload хранится в одном canonical location.
3. Metadata отделена от payload.
4. Самый частый путь должен быть самым дешевым.
5. `check()` не должен читать payload.
6. Raw pointer никогда не является долгоживущим public identity.
7. Handle должен переживать updates, но обнаруживать remove/reuse.
8. `get()` остается простым и безопасным даже после появления fast-path.
9. Flash persistence не должна проникать в application code.
10. Component остается полностью domain-agnostic.

## 20. Ключевая формула v2

```text
key
 ↓ resolve once
handle {slot, generation, version}
 ↓
check metadata many times
 ↓
unchanged → return
changed   → acquire payload directly → use → release
```

Цель `micro_db v2` — дать embedded-коду предсказуемое хранение и очень дешевый доступ к горячему состоянию, не навязывая сложность тем consumers, которым достаточно обычного `get()`.
