# micro_db — Architecture

> **Ревизия 2.** Описание структуры и намерений компонента.
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

**Роль:** keyed mutable records.

**Модель:**

- RAM заранее выделяется на всю capacity; таблица — массив слотов;
- **slot — единая физическая единица хранения: `meta` (заголовок) + `payload`**;
- фиксированный stride: `slot_size = sizeof(meta) + record_size`;
- key адресует slot целиком; слоты переиспользуются;
- runtime — RAM; flash — backing персистентности (по слотам, не перезапись всей таблицы).

Layout:

```text
[meta][payload][meta][payload][meta][payload]...   // фиксированный stride

slot_addr = base + slot * slot_size
meta      = slot_addr
payload   = slot_addr + sizeof(meta)
```

`meta` — **заголовок конкретной записи**, а не отдельная структура/таблица:
`used`, `generation` (та же ли это запись после reuse), `version` (изменились ли
данные).

**Операции — три естественных шага:**

```text
key  → slot
slot → meta          (только заголовок, без payload)
slot → full record   (meta + payload)
```

**За что отвечает:**

| Возможность | Смысл |
|---|---|
| `get / upsert / remove` | базовый copy-out API, безопасный и простой |
| `get_slot(key)` | key → slot |
| `read_meta(slot)` | заголовок **без чтения payload** |
| `read(slot)` | вся запись |
| `iter` | обход записей |

**Слежение за изменениями — забота consumer'а.** Если consumer хочет знать, изменилась
ли запись, он сам хранит `slot + last_generation + last_version` и сверяет с `meta`.
Это его локальное состояние, не сущность micro_db; отдельный `handle` не нужен.

```text
check(slot, seen_generation, seen_version) → CURRENT | CHANGED | REMOVED | STALE
```

**Zero-copy borrow** (`acquire/release`) — не основная цель. Это опциональная
оптимизация, которая держит lock на время чтения и добавляется только по результатам
замеров. Базовый fast-path — `get_slot → read_meta → (при изменении) read`.

**Persistence:** flash-раскладка по слотам, per-slot checksum, declarative-политика
(`RAM` / `RAM + Flash`). При старте — загрузка образа, проверка layout/metadata/CRC,
восстановление RAM и перестройка индексов.

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
3. `meta` — заголовок записи (`record = meta + payload`), а не отдельная структура.
4. Самый частый путь должен быть самым дешёвым.
5. `check()` не читает payload.
6. Raw pointer никогда не является долгоживущей identity; долгоживущее — key.
7. Слежение за изменениями — забота consumer'а (`slot + generation + version`); micro_db handles не ведёт.
8. `get()` остаётся простым и безопасным даже при наличии fast-path.
9. Ring — ordered append/overwrite: без key, hash, free-list и remove-by-key.
10. `seq` — долгоживущая identity Ring-записи; slot — внутренняя деталь.
11. Вытесненный seq даёт STALE/NOT_FOUND, а не ошибку жизненного цикла.
12. Flash-персистентность не проникает в application code.
13. Компонент полностью домен-агностичен.

## 7. Контракт (предложение)

Общие соглашения:

- возвращаемое значение — `esp_err_t` (`OK / NOT_FOUND / INVALID_ARG / NO_MEM /
  INVALID_STATE / INVALID_SIZE`);
- экземпляр table/ring — caller-owned структура; жизненный цикл `init / deinit`;
- capacity фиксирована; память выделяется компонентом при `init`;
- у экземпляра один внутренний lock; все операции — thread-safe относительно него.

### 7.1. Table Store

Схема (задаётся caller'ом): `name`, `record_size`, `key_size`, `max_records`,
`backing` (`RAM` / `RAM+Flash`), `flags`, `persist_key`, и колбэки `key_of`,
`key_equals`, `record_equals`.

Операции:

```text
init(table, schema) / deinit(table)
upsert(table, record, *changed, *inserted)
get(table, key, *record)
remove(table, key, *removed)
clear(table) / count(table) / get_stats(table, *stats)
get_slot(table, key, *slot)
read_meta(table, slot, *meta)
get_by_slot(table, slot, *record) / get_by_index(table, index, *record)
iter(table, cb, ctx) / iter_slots(table, cb, ctx)
check(table, slot, seen_generation, seen_version, *status)   // без чтения payload
```

Данные:

```text
meta   = { used, generation, version }        // заголовок записи (record = meta + payload)
status = CURRENT | CHANGED | REMOVED | STALE
```

`handle` не нужен: consumer хранит `slot + generation + version` у себя. `upsert`
определяет `changed` через `record_equals`; `version` растёт только при реальном
изменении payload; `generation` меняется при переиспользовании слота.

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

## 8. Что нужно решить до реализации

1. **Аллокация.** Компонент выделяет сам (v1-стиль, с выбором caps) или принимает
   заранее выделенный буфер. *Предложение: компонент выделяет, caller-буфер — позже.*
2. **Lock и итерация.** `iter` вызывает колбэк под lock — возможна реентрантность в ту
   же таблицу. *Предложение: документировать «колбэк не мутирует эту же таблицу».*
3. **Persistence.** Table — RAM-only или RAM+Flash (per-slot); Ring — RAM-only.
   *Предложение: оставить как декларативный `backing`, без flash-layout v2.*
4. **`list` vs `iter`.** Фильтрация (prefix) в micro_db или на слое Domain.
   *Предложение: micro_db даёт `iter`; `list(filter)` собирает Domain.*

Решено: `record = meta + payload` — единая физическая единица; отдельного `handle`
нет, consumer сам хранит `slot + generation + version`.
