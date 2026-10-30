# HA V3 — целевые слои и владение runtime

> Статус: **target / архитектурный контракт V3**.
> Дополняет [TARGET_ARCHITECTURE.md](TARGET_ARCHITECTURE.md): там описаны
> canonical-модель, потоки фактов и миграция. Здесь — карта слоёв, владельцы
> ресурсов, задачи и правила зависимостей.
>
> Основа — проверенная V2: `esp32-p4_HA_v2/docs/ARCHITECTURE.md`,
> `domain/DOMAIN_API.md`, `domain/DISPATCHER.md`, `domain/JOURNAL.md`,
> `domain/COMMANDS.md`, `services/ZIGBEE.md`, `services/WEB.md`,
> `clients/DISPLAY.md`, `storage/MSTORE.md`.

## 1. Главная идея

V3 не строится «по файлам» и не является набором подмодулей. V3 — это **один
canonical-поток фактов с одним владельцем каждого ресурса**:

```text
Adapter → Domain (state + Journal) → Dispatcher → subscriber inbox → subscriber task
```

- **Domain** — единственный, кто пишет canonical state.
- **Journal** — единственный поток фактов; не Event Sourcing.
- **Dispatcher** — единственный потребитель Journal.
- **Каждый сервис** владеет своей задачей, своим inbox и своей политикой потерь.
- **Adapter** прячет транспорт; наружу отдаёт только canonical-контракт.
- **Composition root** только собирает и запускает; бизнес-логики в нём нет.

Разделение файлов — следствие этих границ. Перенос кода в новый модуль сам по себе
не является архитектурным изменением.

## 2. Слои и ответственности

```text
┌──────────────────────────────────────────────────────────────┐
│ Application / Clients                                        │
│ Automation | Web | Display | System | Wi-Fi                   │
└───────────────────────────────┬──────────────────────────────┘
                                │ canonical API
┌───────────────────────────────▼──────────────────────────────┐
│ Domain                                                       │
│ Entity Store | Journal | Transient Payload | Dispatcher      │
│ Command routing                                              │
└───────────────────────────────┬──────────────────────────────┘
                                │ storage primitives only
┌───────────────────────────────▼──────────────────────────────┐
│ micro-db                                                     │
│ Table / Ring                                                 │
└──────────────────────────────────────────────────────────────┘

Adapters ──► Domain + ha-model        (Zigbee-private снаружи не виден)
Bootstrap ─► все слои                 (единственное место, где известны все)
```

| Слой | Владеет | Не знает |
|---|---|---|
| `ha-model` | canonical-формы, id, layout, чистые lookup/encode-функции | Domain, runtime, transport, UI |
| `micro-db` | Table/Ring, slot/generation/version, RAM/flash примитивы | Domain, Zigbee, UI, Wi-Fi |
| `ha-domain` | Entity Store, Journal, Transient Payload, Dispatcher, command routing | Zigbee, ZCL, UI, Wi-Fi, ESP-IDF |
| `zigbee-devices` | Zigbee-private registry, interview, bindings, ZCL↔canonical, CommandCore | Web, Display, Automation, Wi-Fi |
| `ha-automation` | правила, RAM-индекс триггеров, собственная задача/inbox | Zigbee, Web, Display |
| `ha-web` | canonical wire protocol, framing | Domain state ownership, Zigbee |
| `ha-app::web` | HTTP/WS-сервис, snapshot/delta, per-client delivery | Zigbee, state ownership |
| `ha-app::display` | LVGL client, экраны, собственная задача/inbox | mstore, Zigbee |
| `ha-app::system` | SNTP, GeoIP, погода; публикует canonical state/event | Zigbee, Web, Display |
| `ha-app::wifi` | Wi-Fi radio, scan/connect, Domain-сущности Wi-Fi | Zigbee, Web, Display |
| `ha-app` (bootstrap) | регистрация таблиц/ёмкостей, сборка задач, C-мосты | бизнес-логику |

### 2.1. `ha-model`

Canonical-словарь и ABI: `Entity`, `Property`, `Event`, `Action`, `Capability`,
формы записей, детерминированные id. Не содержит runtime, задач, Domain API,
Zigbee или UI. Сервисы используют модели, не завися от `ha-domain` только ради типов.

На время миграции типы могут оставаться в `ha-domain::canonical`, но целевая
граница — отдельный `ha-model`.

### 2.2. `micro-db`

Только storage primitives:

- Table — keyed fixed-capacity records, slot/generation/version;
- Ring — bounded ordered facts, oldest eviction;
- RAM/flash policy задаёт владелец таблицы, не micro-db.

`micro-db` не знает, что такое Entity, Property, Zigbee или UI. Сервисы не видят
`mstore`/slot/generation. Storage-типы не протекают в публичный Domain API.

### 2.3. `ha-domain`

Инфраструктурное ядро фактов, а не бизнес-логика.

**Entity Store**

- последнее подтверждённое состояние;
- все entity равны; тип выбирает таблицу;
- `put` возвращает `changed`; неизменённая запись не создаёт факт;
- `get`/`iter` — copy-out; никаких указателей в память Domain;
- связи — данные и составные ключи, не relationship-таблицы.

**Journal**

- compact facts: `EntityUpserted`, `EntityRemoved`, `Event`, `CommandSent`, `Error`;
- bounded ring, oldest eviction; gap считается, а не скрывается;
- история не воспроизводится после reboot;
- большие записи в Journal не копируются;
- события не схлопываются.

**Transient Payload**

- best-effort окно для event-like данных;
- payload не является state;
- вытесненный payload читается как stale.

**Dispatcher**

- отдельная задача;
- свой cursor, свой dispatch-lock, не Domain-lock;
- фильтр по canonical kind/source/entity;
- `try_send`/`wake` только копируют факт в inbox подписчика и будят его;
- код подписчика не исполняется;
- при `cursor < oldest` фиксирует gap и продолжает.

**Commands**

- `post(command, target)` маршрутизирует intent executor'у и возвращается;
- executor короткий: копирует args и передаёт в очередь адаптера;
- `COMMAND_SENT` — только audit;
- Domain не хранит pending/future/timeout команды, не ждёт ACK/report;
- pending/desired/retry/operation tracking — adapter-private `CommandCore`;
- команда не меняет state; state меняет только report от устройства.

**Domain API**

- copy-in/copy-out, без borrowed pointers;
- потокобезопасный фасад; мутации сериализованы;
- state и факт пишутся в одном порядке;
- Domain-lock не удерживается во время UI, сокета, flash-write или Zigbee-отправки.

### 2.4. Zigbee adapter

`zigbee-devices` — adapter, а не второй Domain.

Внутри:

- EUI64, endpoints, clusters, attributes, interview, bindings, ZCL;
- private registry и profile selection;
- ZCL↔canonical bridge;
- `CommandCore`, pending/desired, operation correlation;
- adapter-private device lifecycle и ownership.

Наружу:

- canonical Device/Entity/Property/Event/Capability;
- `domain_entity_put` для report;
- `domain_publish` для event;
- executor для `HA_CMD_PROPERTY`.

Вне adapter нет `EUI64`, `endpoint`, `cluster`, `attribute`, `ZCL`, `zb_*`.

### 2.5. Services и clients

- **Automation** — подписчик canonical фактов. Своя задача/inbox. На trigger читает
  актуальное state из Domain, проверяет условия и постит команду через Domain.
  Zigbee напрямую не вызывает.
- **Web** — тупой транспорт canonical-модели. Snapshot + дельты. Per-client TX
  очередь; медленный клиент не блокирует Domain/Zigbee. Source of truth не хранит.
- **Display** — LVGL client. Своя задача/inbox. Начальный экран — разовое чтение
  Domain. `PROPERTY_STATE` обновляет конкретный виджет; пересборка — только при
  изменении состава/имени/capabilities. Domain каждый тик не опрашивает.
- **System** — время, GeoIP, погода; публикует canonical state/event напрямую.
- **Wi-Fi** — владелец радио; scan/known/status — Domain-сущности. Known — flash,
  не NVS. Web/Display не владеют радио.

### 2.6. Composition root

`ha-app` — единственное место, где известны одновременно:

- список entity-таблиц и их ёмкости/backing;
- Zigbee adapter и его executor;
- все сервисы и их задачи;
- C-мосты платформы.

Он регистрирует descriptors, создаёт Domain, запускает Dispatcher и сервисы.
Он не содержит правил автоматизации, проекции виджетов, web-модели, ZCL-логики
или обработки подписчиков.

## 3. Правила зависимостей

```text
micro-db  ◄── ha-domain  ◄── services/clients
                 ▲
                 │
          zigbee adapter ──► ha-model
                 │
ha-model ◄───────┘

ha-app (bootstrap) ──► все слои
```

Запрещено:

- сервис вызывает другой сервис напрямую;
- сервис видит mstore/slot/generation;
- Domain знает Zigbee/UI/Wi-Fi/ESP-IDF;
- adapter вызывает Display/Web/Automation;
- Web/Display хранят source of truth;
- команда меняет `PROPERTY_STATE`;
- state пишется во flash;
- subscriber body исполняется в producer или Dispatcher.

## 4. Runtime-модель

```text
Adapter task:
  report/read → Domain mutation → Journal append
  command executor → adapter queue → radio

Dispatcher task:
  wait → Journal cursor → filter → try_send/wake → continue

Subscriber task:
  inbox → прочитать state/payload из Domain → своя работа
  при необходимости → domain_post(command)
```

- producer не ждёт consumer'а;
- медленный подписчик теряет только свои факты;
- потеря наблюдаема: gap/drop counter, а не «само пройдёт»;
- Domain-lock не держится на `try_send`, UI, сокет, flash или radio;
- state не проходит через глобальную очередь;
- у каждого подписчика своя bounded очередь.

## 5. Три категории данных

| Категория | Где | Что означает |
|---|---|---|
| State | Domain Entity Store, RAM | последнее подтверждённое состояние Property |
| Event | Domain Journal + payload | что-то произошло; state из этого не следует |
| Command | direct executor + `COMMAND_SENT` audit | намерение; не state |

Правила:

- report — единственный источник state;
- команда, ACK и `COMMAND_SENT` не меняют state;
- event не превращается в state;
- `changed=false` не создаёт факт и не будит подписчиков;
- после reboot state пуст и приходит новыми report'ами;
- persistent config — device/entity/capabilities/groups/rules/settings/location/
  known Wi-Fi; state — никогда.

## 6. Потоки

### Report → state

```text
Zigbee report
  → adapter: EUI64/endpoint → entity, ZCL → canonical value
  → Domain: entity_put(PROPERTY_STATE, (entity, property), value)
       changed=false → факта нет
       changed=true  → Journal EntityUpserted
  → Dispatcher → subscriber inbox
  → subscriber читает актуальное state
```

### Event

```text
Zigbee/System
  → Domain: publish canonical event (compact value / payload)
  → Journal Event
  → Dispatcher → subscribers
```

### Command

```text
UI/Automation
  → Domain: post(command, target)
  → executor (zigbee adapter): resolve entity → EUI64/endpoint, encode ZCL, enqueue
  → Journal CommandSent
  → возврат вызывающему; report придёт отдельно
```

## 7. Ёмкости и перегрузка

Ёмкости — часть composition root, не случайные числа в сервисах:

| Таблица | Ёмкость | Backing |
|---|---:|---|
| `PROPERTY_STATE` | 256 | RAM |
| `ENTITY` | 32 | RAM |
| `LOGICAL_DEVICE` | 32 | RAM/flash registry |
| `CAPABILITIES` | 32 | RAM |
| `GROUP` | 16 | flash |
| `GROUP_ITEM` | 128 | flash |
| `GROUP_AUTOMATION` | 32 | flash |
| `AUTOMATION` | 32 | flash |
| `LOCATION` | 4 | flash |
| `SETTINGS` | 1 | flash |
| `WIFI_KNOWN` | 8 | flash |
| Journal | 64 | RAM |
| Transient payload | 8 × 64 B | RAM |

Переполнение:

- state `NoSpace` — явная ошибка, не вытеснение;
- Journal — oldest eviction + gap counter;
- subscriber inbox — локальная потеря + счётчик;
- frame inbox — drop counter + warning;
- command queue — отказ источнику, state не меняется;
- web model — явный лимит + наблюдаемое вытеснение;
- медленный web-клиент — kill session, не stall системы.

## 8. Целевая структура V3

```text
crates/
  ha-model/          canonical vocabulary and ABI only
  micro-db/          storage primitives
  ha-domain/         Entity Store + Journal + Payload + Dispatcher + routing
  zigbee-devices/    adapter-private registry, bridge, CommandCore
  ha-automation/     rules engine; task wrapper in composition root
  ha-web/            protocol/framing; no runtime ownership
  ha-app/            bootstrap, platform C bridges,
                     display client, web service, system, wifi
```

Допустимые временные отклонения:

- canonical-типы пока в `ha-domain::canonical`, пока не выделен `ha-model`;
- Device lifecycle пока внутри `zigbee-devices`, пока нет второго адаптера;
- web/display service остаются в `ha-app`, пока платформенные зависимости
  не позволяют вынести их в отдельные crates.

Эти отклонения должны быть **явно записаны**, а не появляться как случайные модули.
Новый crate создаётся только при наличии отдельного владельца и зависимости.

## 9. Что считается неправильным

- `main` вызывает `dispatch_once`, `display::drain`, `web::drain`,
  `drain_automation`: Dispatcher и подписчики живут в main.
- Domain знает Zigbee/UI/Wi-Fi.
- Zigbee adapter вызывает Display/Web/Automation.
- Web/Display строят второй source of truth.
- Команда оптимистично пишет state.
- State пишется во flash.
- Потеря подписчика/очереди не наблюдается.
- «Рефакторинг» = перенос файлов без изменения владельца runtime.
- Новый слой/абстракция появляется «на будущее» без потребителя.

## 10. Порядок миграции

### Этап 0 — контракт

- Зафиксировать этот документ и `TARGET_ARCHITECTURE.md`.
- Запретить новые архитектурные обходы.
- Считать текущий `main` временным composition root.

### Этап 1 — модель и Domain API

- Выделить `ha-model` или официально зафиксировать `ha-domain::canonical`
  как временный псевдоним.
- Сделать Domain API потокобезопасным.
- Убрать из Domain всё, что не является Entity Store/Journal/Payload/Dispatcher/
  command routing.
- Добавить тесты на конкурентный put/get и порядок state→fact.

### Этап 2 — Dispatcher и подписчики

- Вынести Dispatcher в отдельную задачу; main перестаёт вызывать `dispatch_once`.
- Automation, Web, Display получают собственные inbox и задачи.
- main перестаёт вызывать их drain-функции.
- Добавить счётчики subscriber drops и gap.

### Этап 3 — команды и adapter

- UI/Automation постят команды через Domain.
- Zigbee executor копирует args и уходит в свою очередь.
- `CommandCore`/pending остаются adapter-private.
- Удалить прямое `intake → executor` из main, если оно обходит Domain.

### Этап 4 — storage и Wi-Fi

- Domain/Device service — единственный путь записи canonical config.
- Wi-Fi known AP — Domain-сущность с flash backing, не NVS.
- Zigbee network — `zb_storage`; default NVS не стирается Zigbee.
- Проверить миграцию capacities без потери данных.

### Этап 5 — composition root

- `ha-app` только регистрирует, создаёт и запускает.
- Убрать из main бизнес-логику и projections.
- Разбиение `main`/`display` — следствие, а не цель.

### Этап 6 — нагрузка

- 32 устройства, 8 свойств, отчёты 5–60 с.
- Медленный Display/Web не останавливает producer.
- Ни один report не теряется до Domain.
- Команда не меняет state до report.
- Flash не пишется на state.
- Все потери видны в счётчиках.
- Замерить heap/stack/latency.

## 11. Критерии готовности

- `main` не содержит обработчиков подписчиков и drain-циклов.
- Domain не зависит от Zigbee/UI/Wi-Fi/ESP-IDF.
- Adapter не вызывает клиентов.
- Сервисы не вызывают друг друга.
- State живёт только в RAM Domain.
- Команда не создаёт state и не ждёт ACK/report.
- Report — единственный источник state.
- Медленный подписчик не блокирует Domain/Dispatcher/Zigbee.
- Потери state/inbox/frame/web видны.
- 32 устройства проходят стенд без silent loss.
- Перенос файлов не меняет контракт слоёв.

## 12. Открытые решения

1. `ha-model` как отдельный crate сразу или после стабилизации Domain.
2. Domain synchronization: `Arc<Mutex<Domain>>` с API-façade или владелец-задача
   с command/query каналами. Выбрать до Этапа 2.
3. Выделять ли neutral Device service в отдельный crate при одном адаптере.
   По V2 — да; по YAGNI — отложить до второго адаптера, зафиксировав границу
   внутри `zigbee-devices`.
4. Web service: оставить платформенный сервис в `ha-app`, а `ha-web` — чистый
   protocol crate.
5. Точные размеры inbox Display/Web/Automation и политика web-клиента.
6. Нужна ли история Event для UI сверх bounded Journal.
