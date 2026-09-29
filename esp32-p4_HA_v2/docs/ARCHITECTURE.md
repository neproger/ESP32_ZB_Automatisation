# ESP32 Home Automation v2 — Architecture

> **Ревизия 2.** Описание структуры и ответственностей системы.
> Это **не план реализации**: сигнатуры, типы и API уточняются в коде.

## 1. Обзор

### 1.1. Главный принцип

**Zigbee semantics are authoritative.**

Мы встраиваемся в существующую модель Zigbee и принимаем её как есть. Команда — это
намерение; подтверждённое состояние приходит только из Zigbee; отсутствие репорта —
отсутствие факта, а не ошибка. Поверх Zigbee не вводим optimistic state, корреляцию
command→result, synthetic acknowledgements.

Отсюда три категории данных. Больше ничего поверх не строим.

```text
State   — последнее известное состояние
Event   — что-то произошло, state из этого не следует
Command — намерение что-то сделать
```

### 1.2. Слои

```text
                 Services / Clients
      Zigbee | Automation | Web | Display
                      │
                      ▼
        ┌───────────────── Domain API ─────────────────┐
        │  Entity Store       — state                   │
        │  Journal            — поток фактов            │
        │  Transient Payload  — best-effort runtime     │
        │  Dispatcher         — доставка                │
        └───────────────────────────────────────────────┘
                      │
                      ▼
        ┌──────────────── micro_db ────────────────────┐
        │  Table Store              Ring Store           │
        └───────────────────────────────────────────────┘
```

Три уровня: **хранение** (`micro_db`) → **ядро** (`Domain`) → **потребители**
(`Services` / `Clients`).

### 1.3. Модель взаимодействия

**State** — пришёл факт от устройства:

```text
Zigbee report
  → Zigbee service
  → Domain: upsert entity
  → Entity Store обновлён + Journal: ENTITY_UPSERTED (compact value)
  → Dispatcher
  → subscribers (читают актуальный state из Entity Store)
```

**Command** — намерение:

```text
UI / Automation
  → Domain: post command
  → Domain маршрутизирует напрямую executor'у (Zigbee service)
  → Journal: COMMAND_SENT
  → Dispatcher
```

**Event** — произошло, но state не меняется:

```text
источник
  → Domain: publish event   (+ optional transient payload)
  → Journal: EVENT
  → Dispatcher
```

**Подписка**:

```text
subscriber (contact + filter)
  ← Dispatcher кладёт compact event в inbox
  → читает state из Entity Store / payload через Domain
```

### 1.4. Правила зависимостей

- `micro_db` линкует только Domain.
- Services и Clients зависят только от Domain.
- Никто не зависит от конкретного сервиса.

## 2. micro_db — слой хранения

**Роль:** fixed-capacity storage primitives для embedded. Предсказуемая память,
минимум аллокаций. Полностью домен-агностичен.

**Подслои:**

- **Table Store** — keyed mutable records.
- **Ring Store** — ordered bounded records.

| Подслой | За что отвечает |
|---|---|
| Table Store | `get / upsert / remove / list`, key → slot, fixed capacity, дешёвая проверка изменения без чтения payload |
| Ring Store | append, monotonic seq как identity, overwrite-oldest, чтение по seq в пределах окна |

**Не знает про:** Domain, Journal, Zigbee, Automation, UI, WebSocket.

На micro_db строятся конкретные хранилища Domain: Entity Store (Table),
Journal и Transient Payload (Ring). Persistence, zero-copy и полный API — тема
`MICRO_DB_V2_DRAFT`.

## 3. Domain — ядро

**Роль:** единственная точка физической записи; публикация фактов; маршрутизация команд;
доставка событий подписчикам. Domain **не** исполняет предметные процедуры и **не** знает
предметной области.

**Подслои:**

### 3.1. Entity Store

Последнее известное состояние. Все entity равны; `entity_type` → таблица/schema через
простой registry (+ опциональный validator). Generic CRUD: `get / list / upsert / remove`.

Тот, кто меняет состояние, сам передаёт компактное описание факта (`source`, `value`,
опц. `payload_ref`). Domain не открывает запись, чтобы «догадаться», что писать в Journal.

### 3.2. Journal

Поток фактов. Тонкие записи; большие entity records в Journal не попадают. Категории:

```text
ENTITY_UPSERTED / ENTITY_REMOVED  — изменился state
EVENT                             — произошло, state не следует
COMMAND_SENT                      — Domain передал intent исполнителю
```

Несёт компактный `value` (история/диагностика) и опциональный `payload_ref`.
Bounded ring: при заполнении вытесняется самый старый факт; никто его не удерживает и не
ждёт. События **не** схлопываются. Источник истины — Entity Store, не Journal.

### 3.3. Transient Payload

Escape hatch для event-like данных, где компактного `value` недостаточно, payload нужен
runtime-потребителю и не является persistent state. Best-effort: без ownership / refcount /
release / TTL; вытеснённый payload читается как STALE. Сервисы работают только через
Domain-фасад; ring-API наружу не выходит.

### 3.4. Dispatcher

Отдельная задача-потребитель Journal. Доставляет compact event в inbox подписчика и будит
его; логику подписчика **не** выполняет и **не** ждёт. Единственный потребитель Journal,
поэтому именно он обнаруживает Journal-gap (по `event_id`).

### 3.5. Domain API

Сервисы видят только Domain API: entity CRUD, commands, payload, subscription/event
delivery. Типы `micro_db` (seq, slot, generation, ring) наружу не выходят; доменные
identity — opaque-типы Domain.

**Domain не знает про:** pending UI, lifecycle команд, correlation, ownership/TTL
payload, процедуры создания/удаления устройств, связи между сущностями, роли/permissions.

## 4. Services

**Модель сервиса:** знает свою внешнюю среду; читает Domain; получает события; пишет
обратно; не знает деталей других сервисов.

- **Zigbee service** — держит Zigbee-модель (endpoint/cluster/attribute), присылает
  state и events, исполняет команды. Детали — отдельная тема.
- **Automation service** — подписчик фактов; при срабатывании правила постит команду.
  Работает по текущему состоянию (event = триггер, state = данные); события при этом
  не схлопываются.
- **Web service (BFF)** — адаптер для браузера: projection canonical records → DTO,
  snapshot при подключении, deltas, приём команд/CRUD. Все фронтенд-особенности
  (batching, throttling, формат) живут здесь и не лезут в Domain.

## 5. Clients

- **Display** — осознанное исключение: прямой polling Domain read API ради дешёвых
  LVGL-обновлений. Остаётся клиентом Domain, к `micro_db` не линкуется.
- **Browser** — через Web service.

## 6. Сквозные правила

1. Только Domain физически пишет в store.
2. Никому ничего не гарантируется: факт может быть вытеснен; реакция — задача подписчика.
3. События не схлопываются.
4. Команда — fire-and-forget: передана executor'у, результат не ждём, корреляции нет.
5. Связи сущностей выражаются данными и составными ключами, а не Domain-relations.
6. Domain не расширяем заранее: новый механизм — только под доказанную потребность.

## 7. Нерешённое

Архитектурные решения, ещё не зафиксированные:

- критерий «compact value vs transient payload»;
- гарантии (или их отсутствие) для edge-triggered автоматик;
- модель request/response операций (`read_attr`, scan, `permit_join`);
- консистентность snapshot в Web service при параллельных писателях.
