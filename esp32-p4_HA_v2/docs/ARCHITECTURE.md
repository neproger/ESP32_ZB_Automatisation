# ESP32 Home Automation v2 — Architecture

> **Ревизия 3 (baseline после Steps A–F).** Описание структуры и ответственностей системы.
> Это **не план реализации**: сигнатуры, типы и API уточняются в коде.

## 1. Обзор

### 1.1. Главный принцип

**Canonical device-domain model; transport representation is adapter-owned.**

Верхние слои и бизнес-логика работают только с canonical-моделью:

```text
Logical Device ── Entity ── Property / Capability / Event
                         └─ Action (намерение над Property)
```

- **Identity.** `Entity.id` (`entity_id`) и `Logical Device.id` (`device_id`) — opaque,
  детерминированно выведённые из стабильного внешнего ключа (`source` + `native_id`), а не
  из сетевого адреса. `device_uid/endpoint/cluster/attr/command/zcl_type` верхним слоям
  неизвестны.
- **State** — единственная истина состояния: canonical `PROPERTY_STATE` `(entity_id,
  property_id)`. Raw-состояния транспорта в Domain не хранится.
- **Transport-specifics** (EUI64, endpoint, cluster, attribute, ZCL, network address,
  interview, bindings, raw diagnostics) живут **только внутри `zigbee/`**.

Отсюда три категории данных. Больше ничего поверх не строим.

```text
State   — последнее известное состояние Property
Event   — что-то произошло, state из этого не следует
Command — намерение что-то сделать
```

### 1.2. Слои

```text
   Application / Clients
   Automation | Web | Display | System | Wi-Fi
            │                │
            │        Device service  — canonical Device lifecycle/ownership
            │                │
            └──────────► Domain ◄──────────┐  canonical API (entities, facts, commands)
                          │                │
                       mstore           Adapters
                  (Table / Ring)     Zigbee  (transport-private:
                                      store, interview, bridge, radio)

   ha_model — canonical vocabulary (forms, ids, pure lookups); линкуют все, но не Domain.
```

- **`mstore`** — storage primitives. Домен-агностичен.
- **`Domain`** — ядро: entity store, journal, transient payload, dispatcher, commands.
  Не знает предметной области и транспорта.
- **`Device service`** — нейтральный сервис над Domain: `register / bind / rename /
  remove` + `register_remover(source, …)`. Превращает нормализованный `ha_device_desc_t`
  в canonical Logical Device и ownership Entity. Транспорта не знает.
- **`ha_model`** — словарь canonical форм/идентификаторов и чистые функции. Не слой, не
  runtime-посредник, не линкует Domain.
- **Adapters** — реализация транспорта. Снаружи отдают только canonical contract;
  внутрь прячут физическую модель. Единственный существующий адаптер — `zigbee`; мост
  «ZCL ↔ canonical» находится внутри него.
- **Application / Clients** — Automation, Web, Display, System, Wi-Fi. Знают только
  canonical Device/Entity/Property/Event/Action.

Строгая граница: **вне `zigbee/` нет `EUI64 / endpoint / cluster / attribute / ZCL /
ha_zb_*`**. Единственное исключение — composition root (`app_main`), который собирает и
запускает адаптер наравне с прочими сервисами.

### 1.3. Модель взаимодействия

**State** — пришёл факт от устройства:

```text
Zigbee report
  → Zigbee adapter (private store + bridge ZCL→canonical)
  → Domain: upsert PROPERTY_STATE (entity, property)
  → Journal: ENTITY_UPSERTED (compact value)
  → Dispatcher → subscribers
```

**Command** — намерение:

```text
UI / Automation
  → Domain: post command (HA_CMD_PROPERTY | HA_CMD_DISCOVERY | …)
  → Domain маршрутизирует executor'у (adapter)
  → adapter резолвит entity → физический адрес и кодирует в ZCL
  → Journal: COMMAND_SENT
```

**Event** — произошло, но state не следует (кнопки, тики времени):

```text
источник (adapter / system)
  → Domain: publish canonical EVENT (payload = ha_event_t)
  → Journal: EVENT → Dispatcher → subscribers
```

**Подписка**:

```text
subscriber (filter по canonical типу факта, не по источнику)
  ← Dispatcher кладёт compact event в inbox
  → читает state из Entity Store / payload через Domain
```

### 1.4. Правила зависимостей

- `mstore` линкует только Domain.
- Services и Clients **не зависят друг от друга** и не видят `mstore`.
- Работа с системой — через Domain API; общие canonical-формы — из `ha_model`
  (`RECORD_MODEL.md`). `ha_model` не линкует Domain.
- Adapter не имеет обратной зависимости от Application: Domain не вызывает adapter;
  adapter-specific удаление инициируется через `device_register_remover(source, …)`.
- Application подписывается на canonical **тип факта**, а не на `source`: какой адаптер
  породил Entity/событие — не его дело.
- `ha_model` не именует адаптеры: `source`/`issuer`/`sys_layer` — opaque-числа; свои
  значения объявляет сам адаптер.

## 2. mstore — слой хранения

**Роль:** fixed-capacity storage primitives для embedded. Предсказуемая память,
минимум аллокаций. Полностью домен-агностичен.

**Подслои:**

- **Table Store** — keyed mutable records.
- **Ring Store** — ordered bounded records.

| Подслой | За что отвечает |
|---|---|
| Table Store | storage primitive над массивом slots (`meta + key + payload`), key → slot, fixed capacity, дешёвая проверка изменения без чтения payload |
| Ring Store | append, monotonic seq как identity, overwrite-oldest, чтение по seq в пределах окна |

Table Store не выставляет CRUD с key в центре: key разрешается в физический slot один
раз (`slot_find`), дальше hot-path идёт по slot. Мутирующие операции принимают
`expected_generation` (защита от stale slot); `slot_read` атомарно отдаёт
`meta + key + payload`.

**Не знает про:** Domain, Journal, Zigbee, Automation, UI, WebSocket.

На mstore строятся хранилища Domain: Entity Store (Table), Journal и Transient Payload
(Ring). Детали — `storage/MSTORE.md`.

## 3. Domain — ядро

**Роль:** единственная точка физической записи; публикация фактов; маршрутизация команд;
доставка событий подписчикам. Domain **не** исполняет предметные процедуры и **не** знает
предметной области.

**Подслои:**

### 3.1. Entity Store

Последнее известное состояние. Все entity равны; `entity_type` → таблица/schema через
registry. Generic CRUD: `get / list / upsert / remove`. Кто меняет состояние — сам
передаёт компактное описание факта (`source`, `value`); Domain не «догадывается».

### 3.2. Journal

Поток фактов. Тонкие записи; большие entity records в Journal не попадают. Категории:

```text
ENTITY_UPSERTED / ENTITY_REMOVED  — изменился state
EVENT                             — произошло, state не следует
COMMAND_SENT                      — Domain передал intent исполнителю
```

Несёт компактный `value` (диагностика) и опциональный `payload_ref`. Bounded ring:
вытесняется самый старый факт. События **не** схлопываются. Источник истины — Entity
Store, не Journal. `domain_value_t` (компактное значение факта) — отдельный словарь
Domain, независимый от canonical `ha_value_t` (совпадающая нумерация — контролируемый
ABI-boundary, не объединяем, чтобы Domain не зависел от `ha_model`).

### 3.3. Transient Payload

Escape hatch для event-like данных. Best-effort: без ownership / refcount / release /
TTL; вытеснённый payload читается как STALE. Canonical событие (`ha_event_t`) несётся
именно здесь; Domain его не интерпретирует.

### 3.4. Dispatcher

Отдельная задача-потребитель Journal. Доставляет compact event в inbox подписчика и
будит его; логику подписчика **не** выполняет. Единственный потребитель Journal,
поэтому обнаруживает Journal-gap (по `event_id`).

### 3.5. Domain API

Сервисы видят только Domain API: entity CRUD + дешёвую ревизию (`domain_entity_meta`),
commands, payload, subscription/event delivery. Контракт — `domain/DOMAIN_API.md`.

**Domain не знает про:** pending UI, lifecycle команд, correlation, ownership/TTL
payload, процедуры создания/удаления устройств, связи между сущностями, роли/permissions,
транспорт.

## 4. Services

**Модель сервиса:** знает свою внешнюю среду; читает Domain; получает события; пишет
обратно; не знает деталей других сервисов.

- **Device service** — canonical lifecycle/ownership Logical Device и Entity. Нейтральный;
  `register_remover(source, …)` — generic-механизм для adapter-specific удаления.
- **Zigbee adapter** — единственный транспорт. Внутри: приватный RAM-store устройства и
  топологии endpoint'ов, интервью, bindings/reporting, мост «ZCL ↔ canonical», radio
  (spinel UART → RCP `ot_rcp` на C6). Наружу — canonical Device/Entity/Property/Event/
  Capability и executor команды `HA_CMD_PROPERTY`. Детали — `services/ZIGBEE.md`.
- **Automation service** — подписчик canonical фактов (EVENT/STATE), правила `ha_rule_t`
  (`entity_id`-адресованные); при срабатывании постит `HA_CMD_PROPERTY`. Детали —
  `services/AUTOMATION.md`.
- **System service** — точное время (SNTP), GeoIP, погода; публикует canonical
  `PROPERTY_STATE` системной Entity напрямую (ZCL не касается) и события-тики. Детали —
  `services/SYSTEM.md`.
- **Wi-Fi service** — владелец радио на внешнем C3 (ESP-Hosted): стек, скан, подключение.
  Сущности `wifi_scan/known/status` с ключом `device_id`. Детали — `services/WIFI.md`.
- **Web service** — тупой транспорт canonical-модели: HTTP+WS, canonical records verbatim
  (`services/WEB_PROTOCOL.md`): snapshot, дельты, команды; интерпретация — в браузере.
  UI встроен в прошивку.

## 5. Clients

- **Display** — прямое чтение canonical (`PROPERTY_STATE`, `LOCATION/WEATHER/WIFI/SETTINGS`)
  ради дешёвых LVGL-обновлений; к `mstore` не линкуется. Экраны — `group`/`group_item`.
  Host-превью — с фейковым Domain (`clients/DISPLAY.md`).
- **Browser** — через Web service.

## 6. Сквозные правила

1. Только Domain физически пишет в store.
2. Никому ничего не гарантируется: факт может быть вытеснен; реакция — задача подписчика.
3. События не схлопываются.
4. Команда — fire-and-forget: передана executor'у, результат не ждём, корреляции нет.
5. Связи сущностей выражаются данными и составными ключами, а не Domain-relations.
6. Domain не расширяем заранее: новый механизм — только под доказанную потребность.
7. Ошибка создаётся слоем, в котором возникла, и проходит без перекодирования (`ERRORS.md`).
8. Zigbee-словарь не выходит за `zigbee/`: вне него нет `EUI64/endpoint/cluster/attr/ZCL`.

## 7. Структура документации

```text
docs/
├── ARCHITECTURE.md          — карта: что существует и зачем
├── RECORD_MODEL.md          — единая модель записей
├── PROPERTY_MODEL.md        — canonical Device/Entity/Property/Action/Event
├── PROPERTY_MODEL_IMPL_JOURNAL.md — журнал миграции (план и чекпоинты)
├── ERRORS.md                — единая модель ошибок
├── STATUS.md                — где мы
├── domain/                  — контракт ядра (DOMAIN_API, ENTITY_STORE, JOURNAL, …)
├── services/                — контракты сервисов/адаптеров
├── clients/                 — Display
├── hardware/                — плата
└── storage/                 — mstore
```

Правило разделения:

```text
ARCHITECTURE.md           → что существует и зачем
layer/component document  → контракт и поведение слоя
code                      → конкретная реализация
```

Детали не дублируются. Открытые вопросы — в соответствующем layer-документе.
