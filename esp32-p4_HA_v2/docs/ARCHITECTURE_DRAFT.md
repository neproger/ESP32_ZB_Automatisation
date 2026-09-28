# ESP32 Home Automation v2 — Architecture Draft

> Статус: **черновик, открыт для обсуждения.**
> Это не финальный контракт. Спорные места помечены `(обсуждается)`.

## 0. Ядро в одной фразе

```text
Entity Store  — хранит последнее известное состояние
Journal       — системный поток фактов: короткая история (со значением факта) + уведомление
Dispatcher    — синхронно раздаёт факты подписчикам
Command       — transient intent: доставляется напрямую сервису, Journal пишет COMMAND_SENT
micro_db      — только механика хранения
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

```text
                 clients                              services
         Web (WS) | Display            Zigbee | Automation | Settings | System
                 \                          /
                  \                        /
                   v                      v
        ┌──────────────── Domain ─────────────────┐
        │  Entity Store  (generic CRUD)            │
        │  Journal       (fact stream)             │
        │  Dispatcher    (notify subscribers)      │
        └──────────────────────────────────────────┘
                            │
                         micro_db            (приватна для Domain)
```

Правила зависимостей:

- `micro_db` линкует **только** Domain;
- сервисы и клиенты зависят **только** от Domain;
- никто не зависит от конкретного сервиса.

## 3. Domain

```text
Domain
├── Entity Store   — последнее известное состояние
├── Journal        — поток фактов (история + канал уведомлений)
└── Dispatcher     — синхронная доставка подписчикам
```

Domain **не** знает про:

- pending UI;
- lifecycle команд;
- cause/correlation;
- staging больших payload;
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
    domain_source_t source;   // ZIGBEE | UI | AUTOMATION | SYSTEM | ...
    domain_value_t  value;    // optional компактный snapshot
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

Journal — **системный поток фактов**: сохраняет короткую историю и уведомляет подписчиков.
В нём только то, что уже произошло. Управления в нём нет.

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

### 5.2. Прочее

- Journal — RAM-блоки, capacity compile-time/configurable (старт ~50). Блок заполнен →
  заводим новый, старый пока выбрасываем.
- В будущем старый блок можно асинхронно выгружать во flash/SD как историю, не меняя
  hot-path. **Сейчас эту механику не проектируем.**
- Источник истины — **Entity Store**, не журнал. Это не Event Sourcing.

## 6. Порядок и Dispatcher

Порядок — простой и строгий, last-writer-wins. Конфликт-резолюшн и merge не нужны.

Один инвариант: **изменение Entity Store и запись факта в Journal имеют один
сериализованный порядок**, и следующий writer не может поменять ту же entity раньше,
чем синхронные подписчики обработали предыдущий факт.

Внешний Domain-lock покрывает **только mutations/publication**:

```text
acquire Domain-lock
    mutate Entity Store        (внутри — короткий micro_db lock)
    journal append (факт)
    sync dispatch              (НЕ под micro_db lock)
release Domain-lock
```

Read API во время dispatch:

```text
domain_get() / domain_list()  НЕ берут Domain-lock.
Они берут только внутреннюю защиту соответствующей micro_db table.
```

Это обязательно, иначе subscriber, вызывающий `domain_get()` внутри callback,
сделает deadlock сам себе.

### 6.1. Вложенная публикация (reentrancy)

Основной путь автоматики:

```text
ENTITY_UPSERTED (DEVICE_STATE, source=ZIGBEE)
  → Automation callback
  → domain_post(OFF)
  → COMMAND_SENT
```

`domain_post()` из subscriber callback — **допустимая вложенная публикация** в рамках
уже активной Domain-операции. Повторно Domain-lock не берётся.

Порядок:

```text
#100 ENTITY_UPSERTED
    subscriber Automation
        → command delivered
        → #101 COMMAND_SENT
             → subscribers COMMAND_SENT
    продолжение subscribers #100
```

- вложенный dispatch происходит синхронно внутри внешнего;
- защита от патологической рекурсии — простой depth limit (без новой архитектуры);
- очередь и отдельный dispatch task не нужны.

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
domain_post(command, event_meta)
      ↓
Domain маршрутизирует команду соответствующему service/executor
      ↓
после успешной передачи → COMMAND_SENT в Journal
      ↓
Journal уведомляет своих subscribers
```

`COMMAND_SENT` означает **только одно**: Domain передал команду сервису для отправки.
В нём нет смысла «устройство получило», «применило», «подтвердило». Domain этого не ждёт.

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

Подписчик:

- слушает Journal (фильтр по `kind/entity/source`);
- читает нужную entity через `domain_get()`;
- при необходимости инициировать действие — вызывает `domain_post(command, meta)`
  (допустима вложенная публикация, см. §6.1).

Пример фильтра автоматики:

```text
entity = DEVICE_STATE
source = ZIGBEE
```

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

## 10. UI

### Display

Читает Entity Store напрямую через Domain, своей модели не держит.
Пользуется `handle/check`, чтобы не читать payload без нужды.
Подписан на факты, перечитывает только нужное.

### Web

Физически удалён, держит семантическую реплику:

```text
ENTITY_UPSERTED → domain_get → WS → Browser Store → React
```

Снапшот при подключении + дельты дальше. Каждый факт реплицируется, поэтому реплика
не расходится со store (в т.ч. по timestamp).

### Pending

Локальная механика UI. Domain в ней не участвует.

## 11. micro_db

Storage engine, домен-агностичен. Первый этап — не усложняем:

```text
handle
check(generation/version)
read(copy-out)
```

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

## 13. Zigbee

В core-архитектуре специально **не проектируем**.

Фиксируем только: Zigbee — один из сервисов/подписчиков Domain, команды приходят
к нему **напрямую** (см. §7), а не через Journal. Дальше живёт обычная Zigbee-модель:
report → Zigbee service → Domain: state → Journal: ENTITY_UPSERTED → subscribers.
Детали state machine, адресации, cluster/endpoint-модели и P4↔C6 — отдельная тема.

## 14. Инварианты

1. Только Domain физически пишет в store.
2. `micro_db` линкует только Domain.
3. Сервисы и UI — клиенты Domain с единым интерфейсом.
4. Все entity равны; `entity_type` — только routing (+ optional validator).
5. Journal никогда не хранит большие entity records; может хранить компактный
   `domain_value_t`, переданный caller'ом.
6. Store change и Journal append — один сериализованный порядок; last-writer-wins.
7. Пришедший факт всегда попадает в Journal, независимо от равенства snapshot.
8. Физический update/version записи — только если snapshot реально изменился
   (решение storage, не Domain).
9. Domain-lock покрывает только mutations/publication; read API берёт только lock таблицы.
10. Никаких callbacks под micro_db lock; dispatch после release storage-lock.
11. `domain_post()` из subscriber callback — вложенная публикация без повторного Domain-lock.
12. Command — fire-and-forget; доставляется напрямую сервису; Journal пишет `COMMAND_SENT`
    (+ compact value); корреляции нет; pending — локально в UI.
13. Семантика Zigbee авторитетна: состояние меняется только по факту снизу;
    optimistic state, timeout и корреляция отсутствуют.
14. REMOVE record содержит полный canonical key.
15. Связи — данные/ключи, а не Domain-relations.
16. Domain не расширяем заранее; новый механизм — только под доказанную потребность.
17. Нет публичной функции без реализации.

## 15. Миграция из v1

```text
micro_db           → развиваем (handle / check / read)
gw_model           → Entity Store (generic CRUD + registry)
gw_model_notify    → Journal
gw_proto_bus       → Dispatcher
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
3. **Journal capacity/retention.** Значение по умолчанию (старт ~50) и future
   persistence. Учесть, что chatty-сенсоры дают много фактов; при необходимости
   consumer фильтрует сам.
4. **Request/response.** Отдельная минимальная модель для операций, которым нужен
   ответ. Граница зафиксирована (COMMAND = fire-and-forget), реализация — позже.

Принято (не открытые):
- Journal может хранить компактный `domain_value_t`; большие записи — нет;
- caller передаёт `event_meta {source, value}`, Domain не извлекает журнал из записи;
- generic kinds: `ENTITY_UPSERTED` / `ENTITY_REMOVED` / `COMMAND_SENT`;
- факт → всегда Journal; физический update — только при реальном изменении snapshot;
- reentrancy `domain_post()` — вложенная публикация + depth limit (§6.1);
- `COMMAND_REJECTED` не вводим — ошибку возвращает сам `domain_post()`;
- `updated/changed`, `record_equals/change_equals`, semantic filtering — убраны;
- каскадное удаление — последовательное, атомарность не вводим без concrete invariant;
- handle/generation для slot reuse — вопрос реализации `check()`;
- lifetime args команды — аргументы у исполнителя в момент dispatch.
