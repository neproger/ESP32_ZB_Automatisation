# ESP32 Home Automation v2 — Architecture Draft

> Статус: **черновик, открыт для обсуждения.**
> Это не финальный контракт. Спорные места помечены `(обсуждается)`.

## 0. Ядро в одной фразе

```text
Entity Store  — хранит последнее известное состояние
Journal       — системный поток фактов: короткая история (со значением факта) + источник триггеров
Dispatcher    — раздаёт подписчикам лёгкие триггеры; их логику не выполняет и не ждёт
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

Сервисы симметричны: каждый знает свою внешнюю среду, читает Domain, получает триггеры,
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
├── Journal                 — поток фактов (история + источник триггеров) → micro_db Ring Store
├── Transient Payload Ring  — best-effort runtime payload (редкие event-like факты) → micro_db Ring Store
└── Dispatcher              — отдельная задача-потребитель Journal; рассылает триггеры
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
typedef struct {
    domain_source_t source;            // ZIGBEE | UI | AUTOMATION | SYSTEM | ...
    domain_value_t  value;             // optional компактный snapshot
    micro_db_ring_seq_t payload_ref;   // optional: ссылка на Transient Payload Ring (см. §5.3)
} domain_event_meta_t;

domain_upsert(entity_type, key, record, const domain_event_meta_t *meta);
domain_remove(entity_type, key,           const domain_event_meta_t *meta);
```

Domain сам добавляет `seq / ts / entity / key / op`. Он ничего не вычисляет и не знает,
почему `25` важно.

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
триггеров. В нём только то, что уже произошло. Управления в нём нет.

Journal — клиент `micro_db` **Ring Store**: `domain_event_t` кладётся через
`micro_db_ring_append` и получает monotonic `seq` как identity. Это bounded live ring
(см. §6.2), не keyed table.

```c
typedef struct {
    uint64_t seq;
    uint64_t ts;

    domain_event_kind_t  kind;    // ENTITY_UPSERTED | ENTITY_REMOVED | COMMAND_SENT
    domain_entity_type_t entity;
    domain_operation_t   op;      // UPSERT | REMOVE | <command op>
    domain_source_t      source;  // ZIGBEE | UI | AUTOMATION | SYSTEM | ...

    domain_key_t   key;
    domain_value_t value;         // optional компактный snapshot

    micro_db_ring_seq_t payload_ref;  // optional transient payload (best-effort), иначе NONE
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

Payload хранится в отдельном RAM ring (тот же `micro_db` Ring Store):

```text
Transient Payload Ring
seq 201 → payload A
seq 202 → payload B
...
```

Событие может нести ссылку:

```c
optional micro_db_ring_seq_t payload_ref;
```

Никакого ownership / refcount / release:

- payload живёт, пока не вытеснен новым payload;
- subscriber успел прочитать — хорошо;
- не успел — `STALE`;
- producer никого не ждёт.

Это **best-effort transient runtime data**. Retention payload **не** зависит от consumers.
Для обычных state-based событий payload ring не используется: подписчик читает текущее
состояние из Entity Store (см. §6.1).

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

Уведомление — отдельная задача-Диспетчер, которая читает Journal и рассылает триггеры:

```text
Dispatcher (своя задача)
    читает Journal по seq (свой cursor)
    для каждого факта — триггерит подходящих подписчиков
```

- Dispatcher — **единственный** потребитель Journal; факты идут по `seq`.
- Подписчики **не** выполняются в контексте producer'а и **не** в контексте Диспетчера.
- Диспетчер **не выполняет логику подписчика** и **не ждёт** её: колбек — это только
  триггер, который будит задачу подписчика. Диспетчер продолжает сразу.
- Domain-lock нужен только на mutation/publication и **не удерживается** во время
  работы подписчиков.
- Нет head-of-line blocking писателей: медленный подписчик тормозит только себя.
- Read API (`domain_get/list`) берёт только внутреннюю защиту micro_db table.

Подписчик — это не колбек с логикой, а **контакт**, которому Диспетчер доставляет
лёгкий триггер. Триггер лишь будит задачу подписчика; **данные подписчик берёт не из
факта, а из Domain** — сам запрашивает нужные entity:

```text
Dispatcher → trigger(subscriber)    (пробуждает, логику не выполняет и не ждёт)
                   ↓
             subscriber task просыпается
             → читает нужные entity из Domain (domain_get/list)
             → обрабатывает как знает
             → при необходимости domain_post(command, meta)
```

Это снимает:
- вызов чужой логики в task'е writer'а;
- вложенную рекурсию и depth limit (подписчик постит команду — она попадает в Journal —
  Диспетчер обработает её в общем порядке);
- удержание Domain-lock во время подписчиков.

### 6.1. Что подписчик читает

Подписчик **не читает Journal**. Триггер говорит только «что-то релевантное изменилось»;
подписчик сам обращается к Domain за entity, которые ему нужны:

- источник данных подписчика — **Entity Store** (`domain_get/list`), а не факт;
- компактный `value` в факте нужен для истории/Archive и диагностики, не для реакции;
- поэтому промежуточные состояния могут «схлопываться»: подписчик видит актуальное
  состояние на момент пробуждения, а не каждое событие по отдельности.

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
- Journal **не ждёт** отстающего потребителя (Диспетчер — единственный, кто читает
  Journal). Gap определяется по `seq`, recovery решает сам сервис; потеря промежуточных
  фактов — не ошибка.

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
Диспетчер триггерит подписчиков
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

- подписчик регистрирует контакт (notification / mailbox) и фильтр (`kind/entity/source`);
- Диспетчер доставляет **лёгкий триггер** — он только будит задачу подписчика,
  не выполняет его логику и не ждёт его;
- подписчик просыпается в своём task'е и читает нужные entity **из Domain**, не из Journal;
- свою очередь / backpressure подписчик организует сам;
- при необходимости инициировать действие — `domain_post(command, meta)`.

Пример фильтра автоматики:

```text
entity = DEVICE_STATE
source = ZIGBEE
```

### 8.1. Automation semantics (state-based)

Обычная автоматика работает по **текущему состоянию**, а не по каждому событию:

```text
Journal/Dispatcher → trigger Automation
Automation wakes
→ reads current Entity Store
→ evaluates current conditions
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
- читает Domain и получает триггеры;
- отправляет действия обратно в Domain;
- не знает деталей других сервисов.

### 10.1. Web Service (backend-for-frontend)

Web Service — адаптер для браузера, симметричный Zigbee-сервису.

```text
Zigbee world → Zigbee service → Domain → Dispatcher → Web service → Browser
Browser      → Web service    → Domain → command / entity mutation
```

Он **не** хранит source of truth. Он:

- получает триггер и понимает, какая entity изменилась;
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
Пользуется `handle/check`, чтобы не читать payload без нужды.
Подписан на триггеры, перечитывает только нужное.

### 10.3. Pending

Локальная механика UI. Domain в ней не участвует.

## 11. micro_db

Storage engine, домен-агностичен. Содержит **два независимых primitive**:

```text
Table Store   — keyed mutable records; get / upsert / remove / list
Ring Store    — ordered bounded records; append / get_by_seq / oldest / newest / overwrite-oldest
```

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
10. Диспетчер доставляет подписчику только лёгкий триггер: не выполняет его логику и
    не ждёт её. Подписчик читает нужные entity из **Domain** (не из Journal); read API
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
    потребителя определяется по `seq`.
19. Journal и Transient Payload — клиенты `micro_db` Ring Store; они не entities и не
    проходят через generic entity CRUD.
20. Transient Payload Ring — best-effort: без ownership / refcount / release / TTL;
    вытесненный payload даёт STALE, Domain не блокируется и никого не ждёт.
21. Четыре разных хранения: Entity Store (текущее состояние), Journal Ring (короткая
    последовательность фактов), Transient Payload Ring (временные runtime payload),
    Archive (история вытесненных Journal records). На текущем этапе все — fixed-capacity RAM.

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
