# HA V3 — архитектурный контракт

> Статус: **архитектурный эталон V3**.
>
> **HA V3 = HA V2 Architecture + Rust implementation.**
>
> V3 не развивает архитектуру V2 и не придумывает новую декомпозицию. V3 переносит
> проверенные слои, владение ресурсами, Domain API, потоки данных и семантику
> State/Event/Command из V2. Rust меняет реализацию, память и типы, но не контракт.
>
> Источник истины — V2:
> `esp32-p4_HA_v2/docs/ARCHITECTURE.md`,
> `RECORD_MODEL.md`, `PROPERTY_MODEL.md`,
> `domain/DOMAIN_API.md`, `domain/ENTITY_STORE.md`, `domain/JOURNAL.md`,
> `domain/DISPATCHER.md`, `domain/COMMANDS.md`,
> `services/ZIGBEE.md`, `services/AUTOMATION.md`, `services/SYSTEM.md`,
> `services/WIFI.md`, `services/WEB.md`, `services/WEB_PROTOCOL.md`,
> `clients/DISPLAY.md`, `storage/MSTORE.md`.
>
> План реализации и порядок этапов: [V3_IMPLEMENTATION_PLAN.md](../plan/V3_IMPLEMENTATION_PLAN.md).
>
> Если документ V3 расходится с V2-контрактом, действует V2, пока расхождение не
> оформлено как отдельное платформенное исключение. Такое исключение должно быть
> явным и обоснованным; «так удобнее в Rust» — не обоснование.

## 1. Неподвижные принципы

1. **Один canonical-поток фактов:**
   `Adapter → Domain (state + Journal) → Dispatcher → subscriber inbox → subscriber task`.
2. **Domain — единственный владелец canonical state.** Никто больше не пишет
   `PROPERTY_STATE`.
3. **Journal — не Event Sourcing и не канал команд.** Это короткая bounded-история
   уже произошедших фактов.
4. **Dispatcher не исполняет код подписчиков.** Он фильтрует, копирует факт в inbox
   и будит задачу подписчика.
5. **У каждого сервиса своя задача и свой bounded inbox.** Переполнение — локальная
   потеря сервиса; она наблюдаема.
6. **Команда — fire-and-forget intent.** Она не является state, не ждёт ACK/report и
   не меняет `PROPERTY_STATE`.
7. **Report от устройства — единственный источник state.**
8. **Transport-specifics не выходят за adapter.** Zigbee-словарь (`EUI64`, endpoint,
   cluster, attribute, ZCL, `zb_*`) не существует вне Zigbee adapter.
9. **Canonical-модель общая, но не является runtime-посредником.**
10. **Composition root только собирает и запускает.** Бизнес-логики в нём нет.

## 2. Слои V2, которые переносятся в V3

```text
ha-model
   │  canonical vocabulary and record layouts
   │
Domain ────────────── Services / Clients
   │                    Device service
   │                    Automation
   │                    Web
   │                    Display
   │                    System
   │                    Wi-Fi
   │
micro-db ──────────── RAM / Flash

Adapters ──────────── Domain + ha-model
app_main ──────────── собирает всё
```

| Слой | Ответственность | Владеет | Не знает |
|---|---|---|---|
| `ha-model` | canonical-словарь и ABI | формы `Entity/Device/Property/Event/Action/Capability`, id, чистые функции | Domain, runtime, transport, UI |
| `micro-db` | storage primitives | Table/Ring, slot/generation/version, RAM/flash | Domain, Zigbee, UI |
| `Domain` | ядро фактов | Entity Store, Journal, Transient Payload, Dispatcher, command routing | Zigbee, UI, Wi-Fi, business-логику |
| `Device service` | canonical lifecycle | Logical Device/Entity/ownership, register/bind/rename/remove, remover registry | transport |
| `Zigbee adapter` | физический протокол | EUI64, endpoints, clusters, interview, bindings, ZCL, radio, private registry | Web, Display, Automation, Domain semantics |
| `Automation` | сервис правил | правила, RAM-индекс, задача/inbox | Zigbee, Web, Display |
| `Web` | транспорт canonical-модели | HTTP/WS, snapshot/delta, per-client delivery | state ownership, Zigbee |
| `Display` | LVGL client | экраны, виджеты, задача/inbox | mstore, Zigbee |
| `System` | системный сервис | SNTP, GeoIP, погода; canonical state/event | Zigbee, Web, Display |
| `Wi-Fi` | владелец радио | scan/connect, Wi-Fi Domain-сущности | Zigbee, Web, Display |
| `app_main` | composition root | регистрация таблиц/ёмкостей, запуск задач, C-мосты | бизнес-логику, projections, правила |

`Device service` — отдельный нейтральный слой V2, не часть Zigbee adapter. Он не знает
EUI64/ZCL; adapter регистрирует в нём remover для своего source. Если в V3 сначала
поддерживается только Zigbee, это не отменяет границу: lifecycle/ownership не должны
растворяться в adapter.

## 3. Разрешённые зависимости

```text
mstore/micro-db  ◄── Domain  ◄── services / clients
                       ▲
                       │
              adapters ─┘  (Domain + ha-model)

Device service ──► Domain + ha-model
services/clients ─► Domain + ha-model
app_main ─────────► все слои
```

Правила:

- `mstore`/`micro-db` видны только Domain.
- `ha-model` не зависит от Domain и не является runtime-посредником.
- Domain не включает `ha-model` и не знает конкретные record types; descriptors
  приходят из bootstrap.
- Adapter зависит от Domain и `ha-model`; обратной зависимости нет.
- Сервисы не зависят друг от друга и не видят adapter.
- Web/Display не являются source of truth.
- Только bootstrap знает одновременно adapter и все сервисы.

Запрещено:

- сервис вызывает другой сервис напрямую;
- adapter вызывает Display/Web/Automation;
- Domain знает Zigbee/UI/Wi-Fi/ESP-IDF;
- Domain отдаёт наружу `slot/generation/version`;
- Web/Display читают `mstore`;
- новые traits/абстракции появляются только потому, что в Rust так удобно.

## 4. Runtime-владение

### 4.1. Producer

```text
Adapter task:
  report/read → canonical value → Domain.entity_put(...)
  event       → Domain.publish(...)
  command     → adapter queue → radio
```

- producer пишет state и факт в одном сериализованном порядке;
- producer не вызывает подписчиков;
- `changed=false` не создаёт факт;
- адаптер не держит Domain-lock во время radio/flash.

### 4.2. Dispatcher

```text
Dispatcher task:
  wait signal
  → читает Journal от своего cursor
  → фильтр canonical kind/source/entity
  → try_send(fact) в inbox подписчика
  → wake(subscriber)
  → continue
```

- отдельная задача, свой cursor, свой dispatch-lock;
- не Domain-lock;
- не исполняет subscriber body;
- gap (`cursor < oldest`) считает и продолжает;
- медленный подписчик не блокирует writers.

### 4.3. Subscriber

```text
subscriber task:
  inbox → прочитать state/payload из Domain → своя работа
  при необходимости → Domain.post(command)
```

- inbox принадлежит подписчику;
- потеря при переполнении — локальная и наблюдаемая;
- state читается из Entity Store, а не из Journal;
- мутирующий Domain API не вызывается из `try_push/wake`.

### 4.4. Main

`app_main` не является dispatcher'ом и не вызывает subscriber drain-функции.
Он только собирает систему, запускает задачи и завершает bootstrap.

## 5. Domain API

V3 переносит V2-контракт `DOMAIN_API.md`, а не изобретает новый.

Публичные операции:

- entity: `register`, `put`, `get`, `remove`, `iter`, `meta`;
- payload: `payload_put`, `payload_get`;
- fact/event: `publish`;
- commands: `post(command, target)`;
- subscriptions: `subscribe(filter, try_push, wake)` / inbox.

Инварианты:

- copy-in/copy-out, без указателей в память Domain;
- ключ — opaque байты фиксированной длины; typed layout знает `ha-model`;
- `put` возвращает `changed`; `changed=false` не создаёт факт;
- `remove` публикует `EntityRemoved` с полным key;
- `meta` — opaque версия без чтения payload;
- Domain не знает смысл entity/property;
- state и факт пишутся в одном порядке;
- команда маршрутизируется executor'у и возвращается без ожидания;
- `COMMAND_SENT` — audit, не подтверждение state;
- `ERROR` — event-like факт, не state;
- Domain не хранит pending/future/timeout команды;
- pending/desired/retry/operation tracking — adapter-private.

## 6. State / Event / Command

| Категория | Где | Смысл |
|---|---|---|
| State | Domain Entity Store, RAM | последнее подтверждённое состояние Property |
| Event | Journal + transient payload | что-то произошло; state не следует |
| Command | direct executor + `COMMAND_SENT` | намерение; не state |

- report — единственный источник state;
- команда/ACK/`COMMAND_SENT` не меняют state;
- event не превращается в state;
- события не схлопываются;
- после reboot state пуст, приходит report'ами;
- config persistent; state — никогда.

## 7. Хранилище

V3 сохраняет концепцию V2 `mstore`:

- Table — ключевые fixed-capacity записи;
- Ring — bounded ordered facts;
- meta: `used/generation/version`;
- copy-in/copy-out;
- flash — backing, не меняющий модель;
- CRC/format/recovery — внутри storage;
- layout/размер записи — контракт; менять без отдельного решения нельзя.

Persistent config V2:

- Device registry — adapter-private;
- Logical Device, Entity, Capabilities, Groups, Rules, Settings, Location,
  known Wi-Fi — Domain canonical records;
- `PROPERTY_STATE` — RAM;
- state не пишется во flash;
- Zigbee network — `zb_storage`, не общий NVS;
- app storage — mstore/Domain, не NVS.

Любая потеря ёмкости/flash-записи должна быть явной ошибкой или наблюдаемым счётчиком.

## 8. Canonical-модель и транспорт

`ha-model`:

- Logical Device → Entity → Property/Capability/Event/Action;
- стабильные canonical id;
- fixed ABI/record layout;
- чистые функции (value projection, id derivation);
- не знает adapter.

Adapter:

- ZCL ↔ canonical bridge;
- endpoint/capabilities → canonical;
- report → Domain state;
- event → Domain event;
- command → ZCL;
- private interview/registry/bindings/radio.

Web:

- canonical records verbatim на проводе;
- snapshot + deltas;
- per-client queue;
- не строит DTO и не интерпретирует Domain;
- не хранит source of truth.

Display:

- читает canonical Domain;
- группы/элементы — данные;
- обновляет виджет по `(entity, property)`;
- пересобирает экран только при изменении состава/имени/capabilities.

## 9. Services V2

### Device service

- neutral canonical lifecycle/ownership;
- `register`/`bind`/`rename`/`remove`;
- `register_remover(source, …)` для adapter-specific удаления;
- не знает EUI64/ZCL;
- Zigbee adapter регистрирует свой remover.

### Automation

- подписчик canonical фактов;
- своя задача/inbox;
- на trigger читает актуальное state;
- условия/правила — Domain entity;
- при срабатывании `Domain.post(command)`;
- не вызывает Zigbee напрямую;
- не исполняется в Dispatcher.

### System

- SNTP/GeoIP/погода;
- публикует canonical state/event в Domain;
- не знает Zigbee/Web/Display.

### Wi-Fi

- единственный владелец радио;
- scan/known/status — Domain entities;
- known — flash;
- scan — RAM;
- provisioning с Display идёт командами;
- Web радио не владеет.

### Web

- snapshot/delta canonical records;
- per-client bounded TX queue;
- медленный клиент убивается, Domain не блокируется;
- команды/CRUD → Domain/Device service;
- не source of truth.

### Display

- LVGL task;
- подписка на Domain facts;
- начальный render — разовое чтение Domain;
- widgets по capabilities;
- команды через Domain.

## 10. Composition root

`ha-app`/`app_main`:

- регистрирует entity descriptors: type/key/payload/capacity/backing/persist_key;
- создаёт Domain;
- регистрирует command executors;
- запускает Dispatcher;
- создаёт adapter;
- запускает services/clients;
- поднимает C-мосты (Zigbee, Wi-Fi, display);
- не содержит правил/проекций/обработки фактов.

## 11. Запрещённые архитектурные изменения

Без отдельного обоснования и документа нельзя:

- добавлять новый слой или runtime-посредник;
- объединять Domain и adapter;
- писать state из UI/automation/command;
- хранить state во flash;
- передавать state через глобальную очередь;
- вызывать subscriber body из producer/Dispatcher;
- делать сервис зависимым от другого сервиса;
- отдавать raw ZCL/EUI64/endpoint наружу;
- менять canonical ABI, ключи, record layout;
- менять ёмкости «по удобству»;
- скрывать переполнение/потери;
- заменять простой V2-механизм новым trait/абстракцией без потребителя;
- считать работающий V3-код доказательством правильной архитектуры.

## 12. Правила миграции

1. Эталон — V2 docs, не текущий V3-код.
2. Сначала составить V2 → V3 mapping и отметить `keep/rework/delete/missing`.
3. Проверить каждый компонент по V2-контракту.
4. Переписывать V3-документы только после аудита.
5. Рефакторинг начинать с владения runtime (Domain/Dispatcher/subscribers),
   а не с переноса файлов.
6. Перенос файлов — следствие границы, а не архитектурное изменение.
7. Временные отклонения фиксировать явно, с условием удаления.

## 13. Критерии готовности

- V3-код соответствует V2 layer contracts.
- Domain — единственный writer canonical state.
- State только RAM; persistent config — Domain/mstore.
- Dispatcher — отдельная задача; main не вызывает `dispatch_once`.
- Automation/Web/Display имеют свои задачи/inbox; main не дренирует их.
- Команда не меняет state и не ждёт ACK/report.
- Report — единственный источник state.
- Adapter не вызывает клиентов; сервисы не вызывают друг друга.
- Медленный подписчик не блокирует Domain/Dispatcher/Zigbee.
- Потери state/inbox/frame/web наблюдаемы.
- 32 устройства, 8 свойств, отчёты 5–60 с проходят стенд.
- Ни один V2-контракт не изменён без отдельного решения.

## 14. Соответствие V2 → V3

Текущий аудит и статус каждого компонента:
[V2_TO_V3_MAPPING.md](../audits/V2_TO_V3_MAPPING.md).

Детали canonical-модели и потоков:
[TARGET_ARCHITECTURE.md](TARGET_ARCHITECTURE.md).
