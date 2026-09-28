# ESP32 Home Automation v2 — Architecture Draft

> Статус: **черновик, открыт для обсуждения.**
> Это не финальный контракт. Спорные места помечены `(обсуждается)`.

## 0. Ядро в одной фразе

```text
Entity Store  — хранит последнее известное состояние
Journal       — системный поток фактов: хранит короткую историю и уведомляет подписчиков
Dispatcher    — синхронно раздаёт факты подписчикам
Command       — transient intent: доставляется напрямую сервису, Journal пишет факт COMMAND_SENT
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

### 1.1. Главный принцип (Zigbee-модель)

```text
Команда никогда не является предположением о будущем состоянии.
Состояние меняется только по факту, пришедшему снизу.
```

Domain не ставит timeout на команду, не откатывает optimistic state и не пытается
понять, дошла ли она. Состояние остаётся таким, каким его **последний раз сообщил**
устройство. Если report не пришёл — значит не пришёл.

### 1.2. Store vs Journal

```text
Entity Store = последнее известное состояние
Journal      = поток фактов, которые произошли
```

Store схлопывает историю до последнего состояния. Journal сохраняет последовательность.
Поэтому **повторный факт с тем же значением — отдельное событие Journal**.

Пример: датчик прислал `25`, потом снова `25`:

```text
STATE_REPORTED / temperature / 25
STATE_REPORTED / temperature / 25
```

Store при этом всё время хранит `temperature = 25`. Домен не спрашивает «старое == новое?»
и не решает, будить ли automation. Если конкретному consumer'у нужна дедупликация
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

Domain фиксирует систему, а не управляет ею.

## 4. Entity Store

Всё постоянное состояние — сущности. У всех единый жизненный цикл:

```text
get(entity, key)
list(entity, filter?)
upsert(entity, key, record)
remove(entity, key)
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

Не framework. Никаких pre/post hooks, стадий коммита и фильтрации по изменению.

### 4.2. Запись

Любой `upsert/remove` — это факт. Пользователь изменил automation:

```text
domain_upsert(AUTOMATION, "auto-1", record)
```

Domain пишет запись и фиксирует факт в Journal. Automation Engine подписан на факты
по `AUTOMATION`, делает `domain_get()` и обновляет своё runtime-состояние.

Большие payload (включая automation на несколько КБ) живут **только** в Entity Store
и никогда не копируются в Journal.

## 5. Journal

Journal — **системный поток фактов**: сохраняет короткую историю и уведомляет подписчиков.

В нём только то, что уже произошло. Управления в нём нет.

Тонкая запись:

```text
{ seq, ts, kind, entity, key, op, source }
```

Типы фактов:

```text
STATE_REPORTED  — entity получила состояние/запись (пришёл факт)
ENTITY_REMOVED  — entity удалена
COMMAND_SENT    — Domain передал команду исполнителю
```

(Позже могут добавиться системные события, automation trace и т.п.)

- Payload в журнал **не попадает** — он живёт в Entity Store.
- Подписчику нужны данные → `domain_get(entity, key)`.
- Journal фиксирует **входящий факт независимо** от того, совпадает ли значение
  с предыдущим. Равенство предыдущему — не повод молчать.
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
STATE_REPORTED
  → Automation callback
  → domain_post(OFF)
  → COMMAND_SENT
```

`domain_post()` из subscriber callback — **допустимая вложенная публикация** в рамках
уже активной Domain-операции. Повторно Domain-lock не берётся.

Порядок:

```text
#100 STATE_REPORTED
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
domain_post(command)
      ↓
Domain маршрутизирует команду соответствующему service/executor
      ↓
после успешной передачи → COMMAND_SENT в Journal
      ↓
Journal уведомляет своих subscribers
```

`COMMAND_SENT` означает **только одно**: Domain передал команду сервису для отправки.
В нём нет смысла «устройство получило», «применило», «подтвердило». Domain этого не ждёт.

- Args команды не хранятся в Journal: их получает исполнитель в момент dispatch.
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
   → Journal: STATE_REPORTED
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
- при необходимости инициировать действие — вызывает `domain_post(command)`
  (допустима вложенная публикация, см. §6.1).

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

Для `STATE_REPORTED` subscriber делает `event → domain_get(key)`.
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
STATE_REPORTED → domain_get → WS → Browser Store → React
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

`generation` существует для slot reuse — вопрос реализации `check()`, не архитектурный.
Zero-copy borrow, новый flash-layout — **позже, только по замерам**.
Детали — в `MICRO_DB_V2_DRAFT.md`.

## 12. gw_proto

Не переносим как есть. Сохраняем сильную идею — единый бинарный контракт и минимальные
преобразования — но делим ответственность по слоям:

```text
canonical entity records   — типы хранилища / Domain
event/command envelope     — отдельный универсальный слой
transport framing          — UART SOF/CRC, WS frame
```

- Интерфейс (envelope) — одинаковый, payload-структуры — строго типизированные.
- Сокращаем количество специальных типов и `MSG_*` там, где хватает универсального
  интерфейса. `(обсуждается)` — точный список.

## 13. Zigbee

В core-архитектуре специально **не проектируем**.

Фиксируем только: Zigbee — один из сервисов/подписчиков Domain, команды приходят
к нему **напрямую** (см. §7), а не через Journal. Дальше живёт обычная Zigbee-модель:
report → Zigbee service → Domain: state → Journal: STATE_REPORTED → subscribers.
Детали state machine, адресации, cluster/endpoint-модели и P4↔C6 — отдельная тема.

## 14. Инварианты

1. Только Domain физически пишет в store.
2. `micro_db` линкует только Domain.
3. Сервисы и UI — клиенты Domain с единым интерфейсом.
4. Все entity равны; `entity_type` — только routing, максимум validator.
5. Journal тонкий; payload только в Entity Store.
6. Store change и Journal append — один сериализованный порядок; last-writer-wins.
7. Journal фиксирует входящий факт независимо от равенства предыдущему значению.
8. Domain-lock покрывает только mutations/publication; read API берёт только lock таблицы.
9. Никаких callbacks под micro_db lock; dispatch после release storage-lock.
10. `domain_post()` из subscriber callback — вложенная публикация без повторного Domain-lock.
11. Command — fire-and-forget; доставляется напрямую сервису; Journal пишет только
    `COMMAND_SENT`; корреляции нет; pending — локально в UI.
12. Состояние меняется только по факту снизу; optimistic state и timeout отсутствуют.
13. REMOVE record содержит полный canonical key.
14. Связи — данные/ключи, а не Domain-relations.
15. Domain не расширяем заранее; новый механизм — только под доказанную потребность.
16. Нет публичной функции без реализации.

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
- reentrancy `domain_post()` — вложенная публикация + depth limit (§6.1);
- `COMMAND_REJECTED` не вводим — ошибку возвращает сам `domain_post()`;
- `updated/changed`, `record_equals/change_equals`, semantic filtering — убраны;
- каскадное удаление — последовательное, атомарность не вводим без concrete invariant;
- handle/generation для slot reuse — вопрос реализации `check()`;
- lifetime args команды — аргументы у исполнителя в момент dispatch.
