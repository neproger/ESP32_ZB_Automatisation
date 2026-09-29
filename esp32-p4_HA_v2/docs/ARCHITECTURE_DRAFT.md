# ESP32 Home Automation v2 — Architecture Draft

> Статус: **черновик, открыт для обсуждения.**
> Это не финальный контракт. Спорные места помечены `(обсуждается)`.

## 0. Ядро в одной фразе

```text
Entity Store  — хранит последнее известное состояние
Journal       — системный поток фактов: короткая история (со значением факта) + источник событий
Dispatcher    — доставляет подписчикам compact domain_event_t; их логику не выполняет и не ждёт
Command       — transient intent: синхронно доставляется сервису, Journal пишет COMMAND_SENT
micro_db      — только механика хранения (Table Store + Ring Store)
```

`Domain` должен оставаться объяснимым ровно этим. Если модели реально не хватает —
поднимаем вопрос отдельно и добавляем **один** механизм, а не наращиваем Domain заранее.

## 1. Принцип

```text
> Если слой нельзя объяснить несколькими простыми правилами — слой слишком большой.
```

`Domain` не знает предметной области. Для него все entity **равны**:
`device`, `automation`, `settings`, `group` — одно и то же. Различается только payload.

`entity_type` нужен только чтобы через простой registry выбрать нужную таблицу/schema.
Если у конкретного типа есть обязательный invariant — допускается validator.
Никаких pre/post hooks, handler chains и скрытого special behavior.

Domain **не** исполняет предметные процедуры. Он хранит entities, публикует факты
и маршрутизирует commands.

### 1.1. Zigbee semantics are authoritative

Мы не строим абстрактную «идеальную» систему управления устройствами.
Мы встраиваемся в уже существующую модель Zigbee и принимаем её семантику такой,
какая она есть.

> Zigbee semantics are authoritative.
>
> Система v2 не пытается заменить или исправить модель Zigbee собственной транзакционной
> семантикой. Она адаптируется к существующей экосистеме: команды передаются как intent,
> фактическое состояние принимается только из входящих Zigbee-событий/репортов, а Journal
> фиксирует наблюдаемые факты и отправленные команды. Поверх Zigbee не вводятся optimistic
> state, command/result correlation, synthetic acknowledgements или другие механизмы, если
> их прямо не предоставляет сама Zigbee-модель.

Прямые следствия:

```text
команда — это просто намерение, переданное в Zigbee-слой;
подтверждённым считается только то, что реально пришло обратно из Zigbee;
отсутствие репорта — отсутствие нового факта, а не «ошибка транзакции»;
повторный репорт того же значения — отдельный факт;
Domain не предсказывает, не подтверждает, не коррелирует и не откатывает состояние;
если Zigbee разделяет устройство на endpoint/cluster/attribute —
    сохраняем эту модель, а не уплощаем её под удобство UI;
асинхронность, задержки, повторные отчёты и fire-and-forget команды —
    не маскируем дополнительными слоями.
```

Domain не ставит timeout на команду и не откатывает optimistic state. Состояние остаётся
таким, каким его **последний раз сообщил** Zigbee. Если report не пришёл — значит не пришёл.

Ограничитель: как только появляется идея вроде «считать лампу включённой сразу после
команды» или «ждать ответ и откатывать» — проверяем: это реально часть Zigbee или мы
опять начинаем строить свою систему поверх неё.

### 1.2. Store vs Journal

```text
Entity Store = последнее известное состояние
Journal      = последовательность фактов, которые произошли
```

Store схлопывает историю до последнего состояния. Journal сохраняет последовательность.
Поэтому **повторный факт с тем же значением — отдельное событие Journal**.

Событие и состояние — разные вещи:

```text
пришёл факт        → всегда Journal
изменился snapshot → только тогда физический update/version записи
```

micro_db может решить, что физически переписывать одинаковый payload не нужно — это его
механика. Domain не связывает Journal с условием «snapshot изменился».

Пример: датчик прислал `25`, потом снова `25`:

```text
ENTITY_UPSERTED / DEVICE_STATE / temperature / 25
ENTITY_UPSERTED / DEVICE_STATE / temperature / 25
```

Store всё время хранит `temperature = 25`. Если конкретному consumer'у нужна дедупликация
повторяющихся событий — он фильтрует их сам.

## 2. Слои

Сервисы симметричны: каждый знает свою внешнюю среду, читает Domain, получает события,
отправляет действия обратно в Domain и не знает деталей других сервисов.

```text
   Zigbee world     Browser (WS)      Automation
        │                │                │
   Zigbee service    Web service     Automation service
        │                │                │
        └────────────────┼────────────────┘
                         v
        ┌──────────────── Domain ─────────────────┐
        │  Entity Store   (generic CRUD → Table)   │
        │  Journal        (fact stream  → Ring)    │
        │  Payload Ring   (transient    → Ring)    │
        │  Dispatcher     (notify subscribers)     │
        └──────────────────────────────────────────┘
                         │
        micro_db  (Table Store + Ring Store)     (приватна для Domain)

        Display ── direct read ──► Entity Store
```

Правила зависимостей:

- `micro_db` линкует **только** Domain;
- сервисы (Zigbee / Automation / Web / ...) и Display зависят **только** от Domain;
- никто не зависит от конкретного сервиса.

## 3. Domain

```text
Domain
├── Entity Store            — последнее известное состояние            → micro_db Table Store
├── Journal                 — поток фактов (история + источник событий) → micro_db Ring Store
├── Transient Payload Ring  — best-effort runtime payload (редкие event-like факты) → micro_db Ring Store
└── Dispatcher              — отдельная задача-потребитель Journal; доставляет события
```

Entity Store использует keyed Table Store. Journal и transient payload используют Ring
Store. У них разная семантика, и они **не** проходят через generic entity CRUD: Journal и
transient payload — не entities.

Domain **не** знает про:

- pending UI;
- lifecycle команд;
- cause/correlation;
- ownership / refcount / release / TTL transient payload;
- процедуры создания/удаления устройств;
- связи (relationships) между сущностями;
- роли, capabilities, permission-модели.

### 3.1. Domain API — что видят сервисы

Сервисы зависят только от Domain и не должны знать типы и механику `micro_db`. Граница:

```text
Application services
        ↓
Domain API
├── entity CRUD
├── command API
├── payload_put / payload_get
└── subscription / event delivery API
        ↓
internal Domain implementation
        ↓
micro_db
├── Table Store
└── Ring Store
```

`micro_db` остаётся приватной storage-механикой Domain. Сервисы **не** должны знать:

```text
ring cursor
slot
generation
micro_db seq type (micro_db_ring_seq_t)
ring contains
oldest / newest
```

`micro_db` может иметь богатый low-level API, но application services видят только
простой Domain API под свою задачу. Если позже сервису понадобится более сложная
операция — она добавляется в Domain API отдельно, по реальной необходимости, а не
через прямой доступ к `micro_db`.

## 4. Entity Store

Всё постоянное состояние — сущности. У всех единый жизненный цикл:

```text
get(entity, key)
list(entity, filter?)
upsert(entity, key, record, event_meta)
remove(entity, key, event_meta)
```

Различаются только типы и payload.

```text
device
endpoint
device_state
group
group_item
automation
settings
```

### 4.1. Entity Registry

Простая routing-таблица:

```text
entity_type → { micro_db table/schema, optional validate }
```

Не framework. Никаких pre/post hooks, стадий коммита и schema-хуков для журнала.

### 4.2. Кто описывает журнальный факт

Domain не должен сам извлекать «что интересно для журнала» из записи. Тот, кто
инициирует изменение, уже знает контекст и передаёт компактное описание факта.

```c
typedef uint64_t domain_payload_ref_t;   // доменный opaque-тип; Domain сам мапит на ring seq/ref

typedef struct {
    domain_source_t source;            // ZIGBEE | UI | AUTOMATION | SYSTEM | ...
    domain_value_t  value;             // optional компактный snapshot
    domain_payload_ref_t payload_ref;  // optional: ссылка на Transient Payload Ring (см. §5.3)
} domain_event_meta_t;

domain_upsert(entity_type, key, record, const domain_event_meta_t *meta);
domain_remove(entity_type, key,           const domain_event_meta_t *meta);
```

Domain сам добавляет `event_id / ts / entity / key / op`. Внутренне `event_id` мапится на
ring seq, но наружу отдаётся только как `domain_event_id_t`. Domain ничего не вычисляет и
не знает, почему `25` важно.

Примеры:

```text
Zigbee report temperature = 25
    domain_upsert(DEVICE_STATE, key, record, { source = ZIGBEE, value = F32(25) })

UI изменил automation
    domain_upsert(AUTOMATION, "auto-7", big_record, { source = UI, value = NONE })
```

Большие entity records (включая automation на несколько КБ) живут **только**
в Entity Store и в Journal не копируются.

## 5. Journal

Journal — **системный поток фактов**: сохраняет короткую историю и служит источником
событий. В нём только то, что уже произошло. Управления в нём нет.

Journal — клиент `micro_db` **Ring Store**: `domain_event_t` кладётся через
`micro_db_ring_append` и получает monotonic identity. Это bounded live ring (см. §6.2),
не keyed table. Наружу отдаётся доменный `domain_event_id_t`; внутренний ring seq —
деталь реализации.

`domain_event_t` — **compact runtime descriptor факта** (он же и запись истории Journal).
Он сам несёт достаточно информации, чтобы subscriber понял: что произошло; к какой
entity/key относится факт; есть ли optional transient payload.

```c
typedef uint64_t domain_event_id_t;    // identity факта Journal

typedef struct {
    domain_event_id_t event_id;       // identity этой Journal-записи
    uint64_t          ts;

    domain_event_kind_t  kind;        // ENTITY_UPSERTED | ENTITY_REMOVED | COMMAND_SENT
    domain_operation_t   op;          // UPSERT | REMOVE | <command op>
    domain_source_t      source;      // ZIGBEE | UI | AUTOMATION | SYSTEM | ...

    domain_entity_type_t entity;
    domain_key_t         key;

    domain_value_t       value;         // optional компактный snapshot (history / diagnostics)

    domain_payload_ref_t payload_ref;   // NONE/null если transient payload отсутствует
} domain_event_t;
```

Типы фактов:

```text
ENTITY_UPSERTED  — сущность получила новое состояние/запись
ENTITY_REMOVED   — сущность удалена
COMMAND_SENT     — Domain передал команду исполнителю
```

`source` различает происхождение (`ZIGBEE` / `UI` / `AUTOMATION` / ...). Отдельного
специального `STATE_REPORTED` в generic Domain нет: Zigbee-состояние — это
`ENTITY_UPSERTED, entity = DEVICE_STATE, source = ZIGBEE`.

### 5.1. Что Journal хранит

> Journal **никогда** не хранит большие entity records. Он может хранить **компактный
> snapshot значения**, необходимый для понимания исторического факта.

Почему: `domain_get()` возвращает только последнее состояние. Без значения событие
через час теряет смысл (`#100 25`, `#101 26` → уже неотличимы).

```text
Zigbee report: temperature = 25
        ↓
domain_upsert(DEVICE_STATE, record, value = 25)
        ↓
Store: temperature = 25
Journal: #101 ENTITY_UPSERTED / DEVICE_STATE / temperature / 25
```

`domain_value_t` — маленький универсальный variant:

```text
NONE
BOOL
I32
U32
F32
ENUM
```

(Позже, при реальной необходимости, — небольшой fixed byte/string. Строковые
human-readable `description` не храним: это форматирование/локализация/heap.
Events UI строит читаемый вид из структуры: `DEVICE_STATE / temperature / 25`.)

Для automation value = `NONE`: сам факт «auto-7 была записана» уже достаточен,
подробности остаются в Entity Store.

`value` сохраняет роль компактного значения **для истории/диагностики**; это не обязательно
бизнес-input автоматики. Если компактного `value` недостаточно, а payload реально нужен
runtime-потребителю, факт несёт `payload_ref` в Transient Payload Ring (см. §5.3).

### 5.2. Прочее

- Journal — bounded RAM ring, capacity compile-time/configurable (старт ~50). При
  заполнении вытесняется самая старая запись (см. §6.2). Подписчики журнал не удерживают.
- Вытесненные записи уходят в RAM Archive (§6.2); выгрузка completed batch во flash/SD —
  позже, в уже зафиксированную точку. Hot-path от этого не меняется.
- Источник истины — **Entity Store**, не журнал. Это не Event Sourcing.

### 5.3. Transient Payload Ring

Отдельный **escape hatch**, не обязательный путь для всех событий. Нужен для фактов, где:

- компактного `domain_value_t` недостаточно;
- payload нужен runtime-потребителю;
- payload не является persistent/current Entity State.

Payload хранится в отдельном RAM ring (`micro_db` Ring Store), но сервисы **не** работают
с ring-API напрямую. Есть тонкий Domain-фасад:

```c
esp_err_t domain_payload_put(
    const void *payload,
    size_t size,
    domain_payload_ref_t *out_ref);

esp_err_t domain_payload_get(
    domain_payload_ref_t ref,
    void *out_payload,
    size_t out_size,
    size_t *out_actual_size);
```

Семантика:

```text
domain_payload_put()
→ append в Transient Payload Ring
→ возвращает opaque domain_payload_ref_t

domain_payload_get()
→ читает payload по ref
→ OK, если payload ещё жив
→ STALE / NOT_FOUND, если уже вытеснен
```

Никакого release / retain / refcount / ownership / TTL / consumer tracking:
payload живёт, пока его не вытеснит ring.

Flow:

```text
Zigbee service receives event-like payload
        ↓
domain_payload_put(payload)
        ↓
domain_payload_ref_t ref
        ↓
domain publishes Journal fact with optional payload_ref
        ↓
Dispatcher delivers compact domain_event_t to subscriber inbox
        ↓
subscriber receives event:
    - state event              → domain_get(entity, key)
    - payload_ref != NONE      → domain_payload_get(payload_ref)
```

Если `domain_payload_get()` возвращает STALE — это нормальная best-effort семантика.
Для обычных state-based событий payload ring не используется: подписчик получает event
и читает текущее состояние из Entity Store (см. §6.1).

Typed payload (не только bytes+size) можно рассмотреть позже, по реальной необходимости;
сейчас достаточно generic bytes + size.

**Invariant.** Journal record должен оставаться осмысленным для истории и диагностики
даже после того, как связанный transient payload уже вытеснен. `payload_ref` — только
дополнительная runtime-информация.

Хороший пример:

```text
Journal:
  kind = ENTITY_UPSERTED / EVENT
  op/value = DOUBLE_PRESS
  payload_ref = vendor-specific details

после потери payload остаётся понятный факт: DOUBLE_PRESS
```

Плохой вариант:

```text
Journal:
  value = NONE
  payload_ref = вся семантика события
```

После вытеснения payload такой Journal record становится бесполезным.

Правило:

```text
Journal fields/value
→ минимально достаточное историческое описание факта

Transient Payload
→ дополнительные runtime-данные, которые допустимо потерять
```

`value` при этом сохраняет роль history / diagnostics, а не бизнес-input автоматики.

## 6. Порядок и Dispatcher

Порядок — простой и строгий, last-writer-wins. Конфликт-резолюшн и merge не нужны.

Изменение store и запись факта в Journal имеют один сериализованный порядок.
Producer **не вызывает подписчиков** — он только пишет state и факт и возвращается:

```text
producer
    write Entity Store        (короткий micro_db lock)
    append fact в Journal
    return
```

Уведомление — отдельная задача-Диспетчер, которая читает Journal и доставляет
подписчикам compact events:

```text
Dispatcher (своя задача)
    читает Journal по event_id (свой cursor)
    для каждого события — находит подходящих подписчиков
    → кладёт compact domain_event_t в inbox подписчика
    → будит его task
```

- Dispatcher — **единственный** потребитель Journal; события идут по `event_id`.
- Подписчики **не** выполняются в контексте producer'а и **не** в контексте Диспетчера.
- Диспетчер **не выполняет бизнес-логику подписчика** и **не ждёт** её: он только
  доставляет compact `domain_event_t` в локальный inbox/FIFO подписчика и будит task.
  Диспетчер продолжает сразу.
- Domain-lock нужен только на mutation/publication и **не удерживается** во время
  работы подписчиков.
- Нет head-of-line blocking писателей: медленный подписчик тормозит только себя.
- Read API (`domain_get/list`) берёт только внутреннюю защиту micro_db table.

Subscriber получает **сам `domain_event_t`** (compact, без больших payload), а не пустой
wake-up и не только `event_id`. Он не читает Journal напрямую через `micro_db`; event
приходит к нему в inbox. Event используется как trigger/context, а source of truth для
state остаётся Entity Store:

```text
Dispatcher → deliver(domain_event_t) в inbox подписчика   (логику не выполняет и не ждёт)
                   ↓
             subscriber task просыпается с event
             ├─ state event     → domain_get(entity, key) / domain_list(...)
             ├─ event-like      → domain_payload_get(event.payload_ref, ...)
             └─ при необходимости → domain_post(command, meta)
```

Это снимает:
- вызов чужой логики в task'е writer'а;
- вложенную рекурсию и depth limit (подписчик постит команду — она попадает в Journal —
  Диспетчер обработает её в общем порядке);
- удержание Domain-lock во время подписчиков.

### 6.1. Что подписчик читает

Подписчик **не читает Journal напрямую через `micro_db`**. Диспетчер доставляет ему
compact `domain_event_t`; event сам говорит, что произошло и есть ли transient payload.

- для state-based фактов источник данных — **Entity Store** (`domain_get/list`), а не
  event; event нужен как trigger/context;
- `event.value` — для истории/диагностики, не обязан быть business-input автоматики;
- если `event.payload_ref != NONE` — transient details читаются через
  `domain_payload_get(event.payload_ref, ...)` (best-effort, может быть STALE);
- промежуточные состояния могут «схлопываться»: подписчик видит актуальное состояние
  на момент пробуждения, а не каждое событие по отдельности.

### 6.2. Retention и Archive

Live Journal — bounded ring на `micro_db` Ring Store. Его никто не удерживает:
подписчики читают Domain, а не Journal, поэтому вытеснение факта их не ломает.

```text
Live Journal Ring                (micro_db Ring Store, capacity compile-time ~50)
    ↓ evicted oldest event (по одной записи)
RAM Archive                      (фиксированный batch, напр. 50 записей)
    ↓ batch заполнен
Completed Archive Batch
    ↓
[future: async flash/SD persistence]
```

- При вытеснении события уходят **по одной записи** в RAM Archive.
- Archive набирает фиксированный batch (напр. 50 записей); заполненный batch считается
  completed, начинается следующий.
- Запись completed batch во flash/SD **пока не реализуем**: lifecycle фиксируем сейчас,
  физическое сохранение подключается позже в точку после completed batch.
- Archive нужен для истории/диагностики, но **не** для runtime replay.
- **Dispatcher — единственный consumer Journal**, поэтому Journal-gap обнаруживает
  Dispatcher, а не сервис:

  ```text
  Dispatcher expected event_id = X
  Journal oldest event_id      = Y > X
  → Dispatcher обнаружил Journal gap
  ```

  Что делает Dispatcher:

  - записывает diagnostic / counter;
  - продолжает с oldest available;
  - не блокирует producer;
  - state-based subscribers восстанавливают актуальное состояние через Entity Store.

  Journal-gap **не** приписывается сервису. Если у конкретного сервиса переполнился его
  собственный локальный FIFO / inbox — это локальная потеря этого сервиса, не Journal gap.

## 7. Команды

Команда — **transient intent**, а не состояние. Она **не идёт через Journal как канал
доставки**. У неё два независимых аспекта:

```text
delivery path  → напрямую в service/executor
audit path     → тонкая запись COMMAND_SENT в Journal
```

```text
UI / Automation
      ↓
domain_post(command, event_meta)          (синхронно)
      ↓
Domain маршрутизирует команду service/executor
      ↓
executor отправляет команду и сразу возвращает управление
      ↓
COMMAND_SENT в Journal
      ↓
Диспетчер доставляет событие подписчикам
```

`COMMAND_SENT` означает **только одно**: Domain синхронно передал команду сервису для
отправки. В нём нет смысла «устройство получило», «применило», «подтвердило». Domain этого
не ждёт и асинхронно не дожидается.

- Команда отправляется **синхронно**: `domain_post()` возвращается, когда команда передана
  executor'у. Никакого фонового ожидания результата нет.
- Zigbee не даёт надёжного факта «выполнено / не выполнено» — этот факт принимаем как есть.
- **Побочные эффекты строятся на факте изменения состояния entity**, а не на завершении
  команды: не «отправили „включить“ и сидим ждём, когда лампа включится», а «лампа
  сообщила `ON` → `ENTITY_UPSERTED` → подписчики отреагировали» (см. §7.2).
- Args команды не хранятся целиком: их получает исполнитель в момент dispatch.
- Если маленький аргумент важен для истории (`SET_LEVEL 32`) — он передаётся
  в `event_meta.value` и попадает в `COMMAND_SENT`. Иначе журнал «отправили SET_LEVEL»
  мало полезен для дебага.
- Если executor не найден или сразу отклонил команду — `domain_post()` возвращает
  обычную ошибку (`ESP_ERR_NOT_FOUND`, `ESP_ERR_INVALID_ARG`, ...), а `COMMAND_SENT`
  не пишется. Отдельных diagnostic-фактов отказа пока не вводим.
- Отдельного persistent command journal/queue/store нет.

### 7.1. Command Registry

Чтобы «Domain маршрутизирует команду» не было магией, есть маленький механизм:

```text
domain_register_command_handler(command_type, executor_cb)
```

(или эквивалентный статический compile-time registry.)

Domain знает только discriminator `command_type` и callback. Ничего предметного:

```text
COMMAND_ZIGBEE_X → callback X
COMMAND_SYSTEM_Y → callback Y
```

Это не слой, а регистрация.

### 7.2. Никакой корреляции

Корреляцию `command → fact` **не отслеживаем**. Это не HTTP RPC.

```text
UI отправил команду
   → Journal: COMMAND_SENT

позже устройство реально изменилось
   → Domain: state = ON
   → Journal: ENTITY_UPSERTED (DEVICE_STATE, source=ZIGBEE)
```

UI не сопоставляет эти события, а просто отображает текущее состояние.
Pending — **локальная UI-механика**; Domain о нём не знает.

### 7.3. Граница: COMMAND vs request/response

```text
COMMAND = fire-and-forget intent
```

Операции, которым вызывающему реально нужен ответ (`read_attr`, network scan,
`permit_join`, binding table и т.п.), — **другой класс взаимодействия**.
Мы его сейчас **не решаем** и не смешиваем с COMMAND.

## 8. Subscribers

Подписчик — это зарегистрированный **контакт**, а не колбек с бизнес-логикой.

- подписчик регистрирует контакт (notification / mailbox / inbox) и фильтр (`kind/entity/source`);
- Диспетчер доставляет **compact `domain_event_t`** в inbox — он не выполняет логику
  подписчика и не ждёт его;
- подписчик просыпается в своём task'е с event; актуальное состояние читает из Domain,
  а transient payload — через `domain_payload_get(event.payload_ref)`;
- свою очередь / backpressure подписчик организует сам (event кладётся в его локальный FIFO);
- при необходимости инициировать действие — `domain_post(command, meta)`.

Пример фильтра автоматики:

```text
entity = DEVICE_STATE
source = ZIGBEE
```

### 8.1. Automation semantics (state-based)

Обычная автоматика работает по **текущему состоянию**, а не по каждому событию:

```text
Journal/Dispatcher → delivers event to Automation
Automation wakes with event
→ event используется как trigger/context
→ reads current Entity Store via domain_get()
→ evaluates rule against current state
→ if true, posts COMMAND
```

То есть для state-based automation:

```text
Journal event = причина проснуться
Entity Store  = данные для решения
```

Если было `ON` → быстро `OFF`, и Automation проснулась уже на `OFF`, то условие `ON` не
выполняется — это нормальная семантика. Промежуточный state воспроизводить не обязаны.

Если позже появятся действительно event-based Zigbee события, которые нельзя выразить
текущим state (`single_press`, `double_press`, vendor event и т.п.), Automation может
использовать `payload_ref`/transient payload. Но это не заставляет все state events ходить
через payload ring.

Роли, capability system и permission-модели сейчас не вводим.

## 9. Связи сущностей

Domain **не знает** отношений. Связи выражаем данными:

- составные ключи: `endpoint = (device_uid, endpoint)`;
- ссылки в полях: `endpoint.device_uid`, `group_item.device_uid/endpoint`;
- списки внутри записи: `endpoint.in_clusters[]/out_clusters[]`; при необходимости `device.endpoint_count`.

Примеры:

- «найти endpoint'ы устройства» → `list(ENDPOINT, prefix = device_uid)`;
- «найти родителя кластера» → endpoint, в записи которого кластер лежит.

### Удаление (REMOVE)

Для `ENTITY_UPSERTED` subscriber делает `event → domain_get(key)`.
Для `ENTITY_REMOVED` читать нечего — значит Journal record для удаления **обязан
содержать полный canonical key**, достаточный для реакции без payload. Для составных
ключей (`device + endpoint + cluster/...`) это явный invariant.

Каскадное удаление остаётся **последовательным**: серия `ENTITY_REMOVED` — нормальна,
пока не появится конкретный invariant, требующий атомарности.

## 10. Сервисы и UI

Каждый сервис:

- знает свою внешнюю среду;
- читает Domain и получает события;
- отправляет действия обратно в Domain;
- не знает деталей других сервисов.

### 10.1. Web Service (backend-for-frontend)

Web Service — адаптер для браузера, симметричный Zigbee-сервису.

```text
Zigbee world → Zigbee service → Domain → Dispatcher → Web service → Browser
Browser      → Web service    → Domain → command / entity mutation
```

Он **не** хранит source of truth. Он:

- получает event и понимает, какая entity изменилась;
- при необходимости делает `domain_get(entity, key)`;
- строит из canonical records удобный браузеру DTO (projection);
- пушит delta через WebSocket;
- при подключении клиента собирает snapshot через `domain_list/get`;
- принимает команды/CRUD от браузера и переводит их в Domain-вызовы;
- решает фронтенд-особенности: batching, throttling, формат дат, capability projection,
  совместимость версий.

Пример: Domain хранит `device / endpoint / cluster / state`, а фронту нужен собранный view:

```json
{
  "id": "lamp-1",
  "name": "Kitchen",
  "capabilities": { "onoff": true, "level": true },
  "state": { "on": true, "level": 80 }
}
```

Domain про такой view не знает — его собирает Web Service из нескольких entity.

Snapshot и delta — задача Web layer:

```text
WS client connected
   → domain_list(DEVICE / ENDPOINT / STATE / GROUP / ...)
   → serialize DTO
   → SNAPSHOT_BEGIN / records... / SNAPSHOT_END

ENTITY_UPSERTED → domain_get → Web DTO → STATE_DELTA → Browser
```

Разделение моделей:

```text
Domain model → Web projection (DTO) → binary WS
```

а не `Domain struct == WebSocket packet`. Бинарность и эффективность сохраняем,
но проекция — отдельный слой. Все web-специфичные костыли остаются здесь и не лезут
в Domain.

### 10.2. Display

Читает Entity Store напрямую через Domain, своей модели не держит.
Пользуется Domain read facade (`handle/check`), чтобы не читать payload без нужды;
к `micro_db` напрямую не линкуется.
Подписан на события, перечитывает только нужное.

### 10.3. Pending

Локальная механика UI. Domain в ней не участвует.

## 11. micro_db

Storage engine, домен-агностичен. Содержит **два независимых primitive**:

```text
Table Store   — keyed mutable records; get / upsert / remove / list
Ring Store    — ordered bounded records; append / get_by_seq / oldest / newest / overwrite-oldest
```

- Приватный storage для Domain: сервисы не видят `micro_db` API напрямую (§3.1); наружу —
  только простой Domain API.
- **Entity Store** использует Table Store (`handle` / `check(generation/version)` /
  `read(copy-out)`).
- **Journal и Transient Payload** используют Ring Store (`seq` как identity, overwrite
  oldest, вытесненный `seq` → STALE). Это не entities и не проходят через entity CRUD.
- micro_db **может** не переписывать одинаковый payload и не увеличивать version,
  если запись физически не изменилась. Journal всё равно уже получил факт (§1.2).
- `generation` существует для slot reuse — вопрос реализации `check()`, не архитектурный.
- Zero-copy borrow, новый flash-layout — **позже, только по замерам**.
- Детали — в `MICRO_DB_V2_DRAFT.md`.

## 12. gw_proto

Не переносим как есть. Сохраняем сильную идею — единый бинарный контракт и минимальные
преобразования — но делим ответственность по слоям:

```text
canonical entity records   — типы хранилища / Domain
event/command envelope     — отдельный универсальный слой (kind/entity/key/op/source/value)
transport framing          — UART SOF/CRC, WS frame
```

- Интерфейс (envelope) — одинаковый, payload-структуры — строго типизированные.
- Сокращаем количество специальных типов и `MSG_*` там, где хватает универсального
  интерфейса. `(обсуждается)` — точный список.
- Для браузера Web Service строит собственную проекцию (DTO) поверх canonical records.
  Envelope остаётся универсальным внутри; WS-протокол — забота Web Service, Domain ABI
  не равен WebSocket-пакету один-в-один.

## 13. Zigbee

В core-архитектуре специально **не проектируем**.

Фиксируем только: Zigbee — один из сервисов/подписчиков Domain, команды приходят
к нему **напрямую** (см. §7), а не через Journal. Дальше живёт обычная Zigbee-модель:
report → Zigbee service → Domain: state → Journal: ENTITY_UPSERTED → Dispatcher → subscribers.
Детали state machine, адресации, cluster/endpoint-модели и P4↔C6 — отдельная тема.

## 14. Инварианты

1. Только Domain физически пишет в store.
2. `micro_db` линкует только Domain.
3. Сервисы и UI — клиенты Domain с единым интерфейсом.
4. Все entity равны; `entity_type` — только routing (+ optional validator).
5. Journal никогда не хранит большие entity records; может хранить компактный
   `domain_value_t` (передан caller'ом) и/или `payload_ref` на Transient Payload Ring.
6. Store change и Journal append — один сериализованный порядок (короткий lock только
   на запись); last-writer-wins.
7. Пришедший факт всегда попадает в Journal, независимо от равенства snapshot.
8. Физический update/version записи — только если snapshot реально изменился
   (решение storage, не Domain).
9. Подписчики не вызываются в контексте producer'а: Диспетчер — отдельная
   задача-потребитель Journal.
10. Диспетчер доставляет подписчику compact `domain_event_t` в локальный inbox: не
    выполняет его логику и не ждёт её. Подписчик не читает Journal через `micro_db`;
    state читает из Domain, transient payload — через `domain_payload_get`. Read API
    берёт только lock соответствующей таблицы.
11. Command — fire-and-forget; синхронно доставляется напрямую сервису; Journal пишет
    `COMMAND_SENT` (+ compact value); корреляции нет; pending — локально в UI; побочные
    эффекты — на факте изменения состояния entity.
12. Семантика Zigbee авторитетна: состояние меняется только по факту снизу;
    optimistic state, timeout и корреляция отсутствуют.
13. REMOVE record содержит полный canonical key.
14. Связи — данные/ключи, а не Domain-relations.
15. Domain не расширяем заранее; новый механизм — только под доказанную потребность.
16. Нет публичной функции без реализации.
17. Web Service — адаптер для браузера (BFF), не источник истины; snapshot / delta /
    projection — его забота, а не Domain. Domain ABI не равен WS-пакету.
18. Live Journal — bounded ring, никто его не удерживает (подписчики читают Domain).
    Вытесненные факты уходят в RAM Archive по одной записи; gap у отставшего
    потребителя определяется по `event_id`.
19. Journal и Transient Payload — клиенты `micro_db` Ring Store; они не entities и не
    проходят через generic entity CRUD.
20. Transient Payload Ring — best-effort: без ownership / refcount / release / TTL;
    вытесненный payload даёт STALE, Domain не блокируется и никого не ждёт.
21. Четыре разных хранения: Entity Store (текущее состояние), Journal Ring (короткая
    последовательность фактов), Transient Payload Ring (временные runtime payload),
    Archive (история вытесненных Journal records). На текущем этапе все — fixed-capacity RAM.
22. Journal record осмыслен без payload: `value`/fields содержат минимально достаточное
    историческое описание; transient payload — только дополнительные runtime-данные,
    которые допустимо потерять.
23. Сервисы видят только Domain API. Типы `micro_db` (seq, slot, generation, ring API) не
    выходят наружу; `domain_event_id_t` и `domain_payload_ref_t` — доменные opaque-типы,
    не `micro_db_ring_seq_t`.
24. `event_id` и `payload_ref` — независимые identity: первый ссылается на Journal-запись,
    второй — на Transient Payload Ring. Payload не ищется через `event_id`, отдельного
    event-lookup перед payload access нет.

## 15. Миграция из v1

```text
micro_db           → развиваем: Table Store (handle/check/read) + Ring Store
gw_model           → Entity Store (micro_db Table Store + registry)
gw_model_notify    → Journal (micro_db Ring Store)
gw_proto_bus       → Dispatcher
transient payload  → Transient Payload Ring (micro_db Ring Store)
rules engine       → сервис-подписчик
gw_zigbee_uart     → Zigbee-сервис (state-machine, отдельная тема)
Web snapshot+delta → сохраняем
Display direct     → сохраняем, через handle/check
ui_control_ack     → удаляем; pending уходит в UI
```

## 16. Открытые вопросы

Закрываем **явным решением**, а не новым слоем автоматически.

1. **Automation trace.** v1 публикует `GW_PROTO_TRACE_*` (fired / action / error /
   action_index). Как это выражается в тонком Journal: дополнительный `kind` или
   отдельный канал.
2. **gw_proto.** Точный список типов на сокращение и формат универсального envelope.
3. **Journal capacity / Archive persistence.** Live Journal — bounded ring, вытеснение не
   блокируется потребителями (подписчики читают Domain, а не Journal); Archive — RAM
   batches (§6.2). Открыто: размеры ring/batch, поведение Archive при переполнении RAM,
   формат и момент async-выгрузки completed batch во flash/SD. Учесть chatty-сенсоры.
4. **Request/response.** Отдельная минимальная модель для операций, которым нужен
   ответ. Граница зафиксирована (COMMAND = fire-and-forget), реализация — позже.
5. **Snapshot consistency.** Web Service собирает snapshot несколькими `domain_list`.
   Параллельные writers могут дать torn snapshot. Варианты: принять eventual
   consistency (дельты догонят) или version-stamped snapshot.
6. **Ring persistence vs Archive.** `micro_db` Ring Store может получить persistence
   (roadmap micro_db), при этом Domain ведёт RAM Archive вытесненных Journal records.
   Нужно явно решить, кто пишет во flash, и не появляются ли два конкурирующих механизма
   истории.
7. **value vs payload_ref.** Критерий, когда факт несёт компактный `domain_value_t`, а
   когда — transient `payload_ref`, пока не зафиксирован. До решения: по умолчанию compact
   value, `payload_ref` — только для реально event-like данных.

Принято (не открытые):
- Journal может хранить компактный `domain_value_t`; большие записи — нет;
- caller передаёт `event_meta {source, value, payload_ref?}`, Domain не извлекает журнал из записи;
- transient payload доступен сервисам только через `domain_payload_put/get`;
  `domain_payload_ref_t` — доменный opaque-тип, `micro_db_ring_seq_t` наружу не выходит;
- Диспетчер доставляет подписчику compact `domain_event_t`, не пустой wake-up; `event_id`
  и `payload_ref` — независимые identity (event-lookup перед payload access не вводим);
- generic kinds: `ENTITY_UPSERTED` / `ENTITY_REMOVED` / `COMMAND_SENT`;
- факт → всегда Journal; физический update — только при реальном изменении snapshot;
- sync dispatch под Domain-lock — отвергнут; Диспетчер отдельный, подписчики вне producer'а;
- `COMMAND_REJECTED` не вводим — ошибку возвращает сам `domain_post()`;
- `updated/changed`, `record_equals/change_equals`, semantic filtering — убраны;
- каскадное удаление — последовательное, атомарность не вводим без concrete invariant;
- handle/generation для slot reuse — вопрос реализации `check()`;
- lifetime args команды — аргументы у исполнителя в момент dispatch;
- Web Service — BFF для браузера, симметричный Zigbee-сервису; snapshot/delta/projection
  и web-костыли живут у него;
- Journal — bounded ring, никто его не удерживает (подписчики читают Domain);
  вытеснение → RAM Archive batch; persistence completed batch во flash/SD — позже;
  Archive не для runtime replay;
- Journal и transient payload — клиенты `micro_db` Ring Store, не entities и не через
  generic entity CRUD;
- Transient Payload Ring — best-effort, без ownership/refcount/release/TTL; stale —
  нормальная потеря, Domain никого не ждёт;
- zero-copy borrow — не основная цель; сначала handle/check/read(copy-out), borrow
  только по замерам.

## 17. Формула хранения

```text
micro_db Table
    → mutable current state
    → Entity Store

micro_db Ring
    → bounded ordered records
    → Journal
    → Transient Payload
```
