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

**Роль:** keyed mutable records (keyed-таблица с изменяемыми записями).

**Модель:**

- RAM заранее выделяется на всю capacity; запись лежит в своём слоте;
- слоты переиспользуются;
- runtime-представление — RAM; flash — backing для персистентности;
- персистентность по слотам (не перезапись всей таблицы).

**Подмодели:**

- **slot metadata** — `used`, `generation` (та же ли это запись после reuse),
  `version` (изменились ли данные);
- **handle** — lightweight ссылка на запись (`slot` + `generation`), не raw pointer;
- **resolve** — key → handle (медленный путь, выполняется один раз).

**За что отвечает:**

| Возможность | Смысл |
|---|---|
| `get / upsert / remove / list` | базовый copy-out API, безопасный и простой |
| `resolve` | key → handle, дальше без hash-lookup |
| `check(handle)` | дешёвая проверка metadata **без чтения payload** (`CURRENT / CHANGED / REMOVED / STALE`) |
| `read(copy-out)` | чтение записи по handle/слоту |
| `iter` | обход записей |

**Zero-copy borrow** (`acquire/release`) — не основная цель. Это опциональная
оптимизация, которая держит lock на время чтения и добавляется только по результатам
замеров. Базовый fast-path — `resolve → check → read(copy-out)`.

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
3. Metadata отделена от payload.
4. Самый частый путь должен быть самым дешёвым.
5. `check()` не читает payload.
6. Raw pointer никогда не является долгоживущей identity.
7. Handle переживает updates, но обнаруживает remove/reuse.
8. `get()` остаётся простым и безопасным даже при наличии fast-path.
9. Ring — ordered append/overwrite: без key, hash, free-list и remove-by-key.
10. `seq` — долгоживущая identity Ring-записи; slot — внутренняя деталь.
11. Вытесненный seq даёт STALE/NOT_FOUND, а не ошибку жизненного цикла.
12. Flash-персистентность не проникает в application code.
13. Компонент полностью домен-агностичен.
