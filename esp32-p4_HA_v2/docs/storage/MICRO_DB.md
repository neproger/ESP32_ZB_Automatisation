# micro_db — Architecture

> **Ревизия 3.** Описание структуры и намерений компонента.
> Это **не план реализации**: API и типы уточняются в коде.

## 1. Обзор

`micro_db` — набор **fixed-capacity storage primitives** для embedded. Цель —
предсказуемая память и отсутствие лишних аллокаций, а не удобство динамических
контейнеров.

Намерения компонента:

- фиксированные слоты, память выделяется заранее на всю capacity;
- быстрый доступ к горячим данным в RAM;
- опциональная flash-персистентность;
- дешёвая проверка изменения без чтения payload;
- recovery/integrity после reboot;
- полная независимость от предметной области.

Внутри — **два независимых primitive**, отвечающих на разные вопросы:

```text
Table Store   — «какое текущее значение записи с этим key»
Ring Store    — «какие записи последовательно пришли за последнее окно»
```

Это разные модели данных, и они не смешиваются.

## 2. Table Store

**Роль:** storage primitive над **фиксированным массивом slots**, а не мини-БД с
CRUD-интерфейсом.

```text
Table
└── slots[capacity]
    ├── slot 0
    │   ├── meta      // mstore: used / generation / version
    │   ├── key       // логическая identity записи, принадлежит mstore
    │   └── payload   // caller-owned bytes
    ├── slot 1
    │   ├── meta
    │   ├── key
    │   └── payload
    └── ...
```

**Почему не CRUD.** Контракт `upsert / get / remove` делает key центром API. В v3
key нужен в основном для того, чтобы **один раз** разрешить логическую identity в
физический slot; после этого нормальный hot-path идёт напрямую через slot, а не через
key.

### 2.1. Термины

```text
table   — fixed-capacity массив slots
slot    — физическая ячейка table
meta    — служебное состояние slot, принадлежит mstore
key     — логическая identity записи, принадлежит структуре storage
payload — пользовательские данные записи
```

Физически запись в таблице — `meta + key + payload`; наружу эти части отдаются
раздельно.

`key` — логическая identity **всей записи**, а не «часть payload»: по одному и тому же
key через `slot_find` мы получаем slot, а из него — и `meta`, и `key`, и `payload`.
Key **хранится самим storage** (внутри slot), а не извлекается из клиентской структуры
через callback. Это убирает зависимость mstore от внутреннего layout payload.

### 2.2. Физическая модель

- RAM заранее выделяется на всю capacity; таблица — массив слотов;
- **slot = `meta` (заголовок) + `key` + `payload`**;
- фиксированный stride: `slot_size = sizeof(meta) + key_size + payload_size`;
- слоты переиспользуются;
- runtime — RAM; flash — backing персистентности (по слотам, не перезапись всей таблицы).

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
key     → хранится mstore, задаётся caller'ом только при allocate
payload → полностью задаётся caller'ом
```

- Caller передаёт `key` при `slot_allocate` и payload; **никогда** не задаёт и не меняет
  `used / generation / version`.
- Key далее неизменяем для incarnation слота: `slot_update` его не трогает.
- Metadata обновляет mstore.

### 2.4. slot и identity

- `slot` — **не** долговечная identity: он переиспользуется.
- Валидность закешированного slot всегда проверяется через `generation`.
- Долговечная логическая identity — `key`; key разрешается в slot через `slot_find`.
- `key` хранится внутри slot (принадлежит storage) и неизменяем в пределах incarnation.

### 2.5. Canonical state и runtime derived state

Принципиальное разделение: то, что хранится, и то, что вычисляется.

```text
source of truth (canonical):
    slots[].meta.used
    slots[].meta.generation
    slots[].meta.version
    slots[].key
    slots[].payload

runtime derived state (ускорители):
    key index        (key → slot)
    free-list
    live_count
```

Следствие: после reboot или при обнаружении нарушения инварианта весь runtime state
можно выбросить и восстановить простым проходом по slots.

```text
rebuild_runtime():
    for each slot:
        if meta.used:
            index_insert(slot.key, slot_number)
    live_count = число used
    free-list  = слоты с used == 0
```

Индекс, free-list и live_count — **не** source of truth; истина — сами slots, поэтому
любой ускоритель можно удалить и восстановить без изменения поведения хранилища.

### 2.6. generation / version и инварианты

```text
generation
→ защита физического адреса от ABA/reuse
→ cached slot=17 gen=42; slot 17 освободили и выдали другой записи → gen=43
  → кеш перестаёт обозначать старую identity

version
→ относится к текущему incarnation слота
→ меняется только при реальном изменении payload внутри одной generation
```

Базовый инвариант:

```text
allocate:
    used:       0 -> 1
    key:        set (identity новой записи)
    generation: increment
    version:    initial value

update:
    used:       stays 1
    key:        unchanged (identity incarnation неизменяема)
    generation: unchanged
    version:    increment iff payload changed

free:
    used:       1 -> 0
    generation: unchanged

next allocate of same physical slot:
    generation: increment
```

Смена identity — это **не** `slot_update`, а `slot_free` + `slot_allocate`: новая
логическая запись и новая `generation`.

Значение начала (0/1) и поведение при wraparound — вопрос реализации.

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
    key/payload     = stale

// позже тот же физический slot занимает другая запись
slot_allocate(key=77, payload={state=OFF})
slot 5:
    meta.used       = 1
    meta.generation = 14        // новый владелец slot
    meta.version    = 1
    key             = 77
    payload         = {state=OFF}
```

Смысл: `slot` остаётся тем же физическим местом, `generation` показывает смену
логической записи, `version` — изменения внутри текущей записи.

### 2.7. Операции

Ядро — slot-centric, без key в центре hot-path:

```text
slot_find(table, key, *slot)
slot_meta(table, slot, *meta)
slot_key(table, slot, *key)
slot_read(table, slot, *payload)

slot_allocate(table, key, payload, *slot)
slot_update(table, slot, payload)
slot_free(table, slot)

scan / iter / count / clear
```

| Возможность | Смысл |
|---|---|
| `slot_find` | key → slot (через runtime index) |
| `slot_meta` | заголовок **без чтения payload** |
| `slot_key` | key записи по slot |
| `slot_read` | payload по slot |
| `slot_allocate` | занять свободный slot под `(key, payload)` |
| `slot_update` | обновить payload существующего slot (key не меняется) |
| `slot_free` | освободить slot |
| `iter / count / clear` | обход, размер, очистка |

Типовой потребитель (Entity Store):

```text
// один раз
slot_find(key) -> slot

// hot path
slot_meta(slot) -> generation / version

// состояние изменилось
slot_read(slot)
```

**Слежение за изменениями — забота consumer'а.** Consumer хранит `slot + last_generation
+ last_version`, делает `slot_meta(slot)` и сравнивает. Это его локальное состояние, не
сущность micro_db.

**Zero-copy borrow** (`acquire/release`) — не основная цель. Опциональная оптимизация,
добавляется только по результатам замеров.

### 2.8. slot_find и runtime index

В рабочей реализации `slot_find(key)` идёт через runtime index, а **не** сканирует
slots:

```text
key
 ↓
runtime index
 ↓
slot
 ↓
meta / key / payload
```

```text
slot_find(key):
    lookup in index
    if found:
        validate slot.used
        optionally validate key   // защита от рассинхронизации
        return slot
```

Индекс не становится source of truth — он лишь ускоряет `key → slot`. Истина остаётся
в slots, поэтому индекс можно полностью выбросить и восстановить (см. `rebuild` в 2.5).

Линейный проход по slots — **служебный fallback/repair**, а не штатный API-path:
используется для перестройки индекса после старта или когда runtime state признан
неконсистентным. На первом этапе реализации `slot_find` намеренно линейный: это сразу
отделяет семантику Table Store от ускорителя, после чего индекс добавляется так, чтобы
его удаление не меняло поведение хранилища.

```text
slots     — canonical storage
index     — ускорение slot_find
free-list — ускорение allocate
live_count — кэшированная статистика
```

### 2.9. Schema

Storage **владеет key**, поэтому key-часть имеет фиксированный layout. Callback'ов
`key_of / key_equals` не нужно: mstore хранит key в слоте и сравнивает его сам (memcmp
по `key_size`). Schema задаёт размеры:

```c
typedef struct {
    size_t capacity;
    size_t key_size;       // фиксированный layout ключа
    size_t payload_size;

    bool (*payload_equals)(const void *a, const void *b);  // опц.; иначе memcmp
} mstore_table_schema_t;
```

`payload_equals` нужен только чтобы решить, вырос ли `version`; для opaque bytes
достаточно memcmp по `payload_size`.

### 2.10. Persistence

Flash-раскладка по слотам, per-slot checksum. Canonical state персистится целиком
(`meta + key + payload`), чтобы `generation / version` переживали reboot. При старте —
загрузка образа, проверка layout/metadata/CRC и перестройка runtime state (`index /
free-list / live_count`) проходом по slots.

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

## 4. Чего micro_db не делает

- Не знает про: Domain, Journal, Zigbee, Automation, UI, WebSocket, devices.
- Не содержит событий, подписок, callback'ов, маппинга и trigger-логики.
- Не заводит entity-specific CRUD и application-команды.

Любому вызывающему micro_db сообщает только результат операции: `OK / error`,
`changed / inserted / removed`, `metadata`. Вся event-based логика строится слоем выше.

Оба primitive остаются **storage mechanics** и ничего не знают про смысл данных.

## 5. Роль в Home Automation

```text
Home Automation services (Zigbee / Web / Automation / Display)
        ↓
Domain facade
        ↓
micro_db
├── Table Store   → Entity Store
├── Ring Store    → Journal
└── Ring Store    → Transient Payload
```

- Сервисы работают только через Domain facade и **не** видят micro_db API/типы
  (seq, slot, generation, ring-функции наружу не выходят).
- Доменные identity (`event_id`, `payload_ref`) принадлежат Domain; реализация может
  кодировать micro_db seq напрямую — отдельной таблицы сопоставления не требуется.
- micro_db владеет только storage mechanics; Domain владеет смыслом сущностей и событий.

## 6. Намерения (design rules)

1. Fixed memory layout важнее удобства динамических контейнеров.
2. Payload хранится в одном canonical location.
3. `record = meta + key + payload` — единая физическая единица, а не отдельные структуры.
4. Storage владеет `meta` и `key`; caller владеет только `payload`.
5. Самый частый путь должен быть самым дешёвым: key → slot один раз, дальше доступ по slot.
6. `slot_meta()` не читает payload.
7. `slot_update()` не меняет key; смена identity — это `free + allocate` (новая generation).
8. Raw pointer никогда не является долгоживущей identity; долгоживущее — key.
9. Слежение за изменениями — забота consumer'а (`slot + generation + version`); micro_db handles не ведёт.
10. Canonical state — только `slots`; `index / free-list / live_count` — производные и
    восстанавливаемые ускорители.
11. `slot_find` ускоряется index'ом; линейный проход — только rebuild/repair, не API-path.
12. Table Store не выставляет CRUD с key в центре (`upsert / get / remove` не входят в ядро).
13. Ring — ordered append/overwrite: без key, hash, free-list и remove-by-key.
14. `seq` — долгоживущая identity Ring-записи; slot — внутренняя деталь.
15. Вытесненный seq даёт STALE/NOT_FOUND, а не ошибку жизненного цикла.
16. Flash-персистентность не проникает в application code.
17. Компонент полностью домен-агностичен.

## 7. Контракт (предложение)

Общие соглашения:

- возвращаемое значение — `esp_err_t` (`OK / NOT_FOUND / INVALID_ARG / NO_MEM /
  INVALID_STATE / INVALID_SIZE`);
- экземпляр table/ring — caller-owned структура; жизненный цикл `init / deinit`;
- capacity фиксирована; память выделяется компонентом при `init`;
- у экземпляра один внутренний lock; все операции — thread-safe относительно него.

### 7.1. Table Store

Схема (задаётся caller'ом): `capacity`, `key_size`, `payload_size`, `backing`
(`RAM` / `RAM+Flash`), `flags`, `persist_key`, и опциональный колбэк `payload_equals`
(см. 2.9).

Ядро:

```text
init(table, schema) / deinit(table)

slot_find(table, key, *slot)
slot_meta(table, slot, *meta)
slot_key(table, slot, *key)
slot_read(table, slot, *payload)

slot_allocate(table, key, payload, *slot)
slot_update(table, slot, payload)
slot_free(table, slot)

iter(table, cb, ctx)
count(table)
clear(table)
```

Данные:

```text
meta = { used, generation, version }   // заголовок slot, принадлежит mstore
key                                    // identity записи, принадлежит mstore
```

`handle` не нужен: consumer хранит `slot + generation + version` у себя и сверяет с
`meta` из `slot_meta`. `slot_update` определяет `changed` через `payload_equals`;
`version` растёт только при реальном изменении payload; `generation` меняется при
переиспользовании слота, а key в пределах incarnation неизменен.

Всё остальное (например `slot_stats`) добавляется только под конкретного
потребителя.

### 7.2. Ring Store

Конфигурация: `record_size`, `capacity`.

```text
init(ring, config) / deinit(ring) / count(ring)
append(ring, record, *seq)
get_by_seq(ring, seq, *record)               // вытесненный seq → STALE / NOT_FOUND
oldest_seq(ring, *seq) / newest_seq(ring, *seq)
contains(ring, seq, *bool)
```

`seq` — `uint64`, монотонный; slot/cursor — внутренняя деталь.

## 8. Порядок работ и открытые вопросы

RAM-модель Table Store доводится до конца до partition, flash, hash-index и Ring:

```text
1. точный layout slot (meta + key + payload)
2. тип key (фиксированный layout) и его хранение
3. тип slot index
4. semantics used / generation / version
5. allocate / update / free
6. key / read / meta
7. slot_find(key)          // сначала линейный, затем index-backed
8. scan / iter / count / clear
9. инварианты
10. runtime index / free-list
11. persistence
```

Решено: key принадлежит storage и хранится в слоте (fixed layout, без `key_of`
callback); `slot_update` не меняет key.

Открыто:

1. **Аллокация.** Компонент выделяет сам или принимает заранее выделенный буфер.
   *Предложение: компонент выделяет, caller-буфер — позже.*
2. **Lock и итерация.** `iter` вызывает колбэк под lock. *Предложение: документировать
   «колбэк не мутирует эту же таблицу».*
3. **Rehash индекса.** Порог по доле tombstone; пока не проектируется.
4. **Persistence.** RAM-only или RAM+Flash (per-slot); политика write-through vs
   batched.
5. **`iter` vs `list`.** micro_db даёт `iter`; `list(filter)` собирает Domain.
