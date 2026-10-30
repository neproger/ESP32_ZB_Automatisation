# HA V3 — canonical-контракт и потоки данных

> Статус: **детализация canonical-модели и потоков для V3**.
> V3 — это Rust-порт архитектуры V2, а не новая эволюционная ступень.
> Главный архитектурный контракт: [V3_LAYERS.md](V3_LAYERS.md).
> Аудит V2 → V3: [V2_TO_V3_MAPPING.md](V2_TO_V3_MAPPING.md).
> Текущий V3 местами ещё не соответствует эталону V2 (см. mapping).
>
> Основа — проверенная модель V2:
> `esp32-p4_HA_v2/docs/ARCHITECTURE.md`,
> `domain/DISPATCHER.md`, `domain/JOURNAL.md`, `clients/DISPLAY.md`,
> `RECORD_MODEL.md`, `storage/MSTORE_BENCH.md`.
>
> Концептуальный бриф: [ARCHITECTURE_BRIEF.md](ARCHITECTURE_BRIEF.md).
> При расхождении по слоям/runtime действует `V3_LAYERS.md`, по canonical-контракту —
> этот документ, а по архитектурному эталону — V2.

## 1. Главный принцип

**Canonical state — единственная истина; Journal — поток фактов; команда — намерение.**

```text
Logical Device ── Entity ── Property / Capability / Event
                         └─ Action (намерение над Property)
```

Три категории данных не смешиваются:

```text
State   — последнее подтверждённое состояние Property. Живёт в RAM.
Event   — что-то произошло, state из этого не следует. Не хранится как state.
Command — намерение что-то сделать. Не является state и не создаёт ожидание.
```

Отсюда основные инварианты:

1. **Report/event от устройства — единственный источник state.**
   Команда, `COMMAND_SENT`, Default Response, ACK транспорта не меняют state.
2. **Command — fire-and-forget.** Она передаётся исполнителю и забывается.
   Domain не ждёт результата, не держит future/task ожидания и не коррелирует
   команду с будущим report. `COMMAND_SENT` — только audit-факт.
3. **Подписчик реагирует на факт, а не на команду.** Web и Display обновляют
   состояние по `PROPERTY_STATE`/`EVENT`, а не оптимистично по факту отправки.
4. **State не проходит через глобальную очередь.** Он пишется в Entity Store
   сразу. Очередь есть только у подписчика, и она не является хранилищем истины.
5. **Dispatcher не исполняет чужой код.** Он фильтрует, копирует факт в inbox
   подписчика и будит его; затем идёт дальше.

## 1.1. V2 — база, а не источник вдохновения

V3 не проектирует новую модель. Берём V2 как есть:

- те же категории `State / Event / Command`;
- ту же canonical identity и точные ABI-формы
  (`DOMAIN_V2_MAPPING.md`);
- тот же Entity Store как единственную истину;
- тот же Journal + Dispatcher + subscriber inbox;
- тот же adapter boundary (Zigbee-private → canonical);
- ту же политику хранения: state — RAM, persistent config — flash;
- те же правила перегрузки: bounded, с явными потерями/счётчиками.

**V2 не была безлимитной.** `fixed capacity` — её сильная сторона:
предсказуемая память и отсутствие скрытых аллокаций. Меняются только размеры
(на V2-бюджет), а не принцип. «Убрать capacity» означало бы придумать новую
модель и потерять предсказуемость.

Rust меняет **только способ реализации**, не контракт:

- `void*`/runtime registry → типизированные таблицы и `enum`;
- callback + `ctx + wake` → `SyncSender<Fact>` / trait-подписчик;
- ручные `memcpy`/`static_assert` → `#[repr(C)]`, `Copy`, `const`-проверки;
- `sys_error_t` → `Result`/`enum Error` с маппингом на границе;
- ручные lock/free → RAII и `Arc`/`Mutex`.

## 2. Слои

```text
Application / Clients
  Automation | Web | Display | System | Wi-Fi
        │
        ▼
      Domain  ── Entity Store (RAM) + Journal + Dispatcher + Commands
        │
        ▼
     micro-db (RAM/FLASH primitives; не знает Domain/Zigbee)
        ▲
        │
Adapters
  Zigbee (ZCL, EUI64, endpoint, cluster, interview, bindings)
```

- **Domain** не знает Zigbee, UI, MQTT и предметную семантику.
- **Adapter** прячет EUI64/endpoint/cluster/attribute/ZCL; наружу отдаёт только
  canonical Entity/Property/Event/Action.
- **ha-model** — общий словарь canonical-форм и идентификаторов.
- **micro-db** — только storage primitives; сервисы не видят `slot/generation`.
- **Composition root** (`ha-app`) собирает Domain, адаптер и сервисы.

## 3. Потоки данных

### 3.1 Report → state

```text
Zigbee report
  → Adapter: private lookup (EUI64, endpoint) → entity_id
  → Adapter: ZCL → canonical property/value
  → Domain: entity_put(PROPERTY_STATE, (entity, property), value)
       ├─ payload не изменился → changed=false, факта нет
       └─ payload изменился   → ENTITY_UPSERTED в Journal
  → Dispatcher: фильтр → try_push(event) в inbox подписчиков
  → Subscribers: читают актуальный state из Entity Store
```

Правила:

- state пишется **сразу**, а не через промежуточную очередь состояний;
- `changed=false` не создаёт факт и не будит подписчиков;
- `PROPERTY_STATE` — RAM; после reboot state восстанавливается новыми
  report'ами, а не из flash;
- operational metadata (`last_seen`, `rssi`, `lqi`) не входит в canonical state,
  иначе `changed` всегда true; для неё отдельный тип/сервис или RAM.

### 3.2 Event

```text
Источник (Zigbee/System)
  → Domain: publish canonical EVENT (payload = ha_event_t)
  → Journal: EVENT
  → Dispatcher → subscribers
```

Событие — самостоятельный факт. Оно не превращается в свойство и не
схлопывается с соседними событиями: `on on on off` — четыре факта.
После reboot события не воспроизводятся.

### 3.3 Command

```text
UI / Automation
  → Domain: post(command, target)
  → Domain: executor(command) синхронно передаёт intent
  → Adapter: canonical → ZCL, enqueue в транспорт
  → Domain: COMMAND_SENT (audit)
  → возврат вызывающему
```

- вызывающий **не ждёт** ни доставки, ни применения, ни report;
- Domain не хранит `pending command`, `future`, `oneshot` или `timeout`
  команды как часть state;
- транспортная надёжность (TSN/retry/ACK) — **внутри adapter'а**, не в Domain;
- adapter-private operation tracking допустим, но он не является Domain state
  и не блокирует UI/Automation;
- Default Response/ACK не пишет `PROPERTY_STATE`. Если состояние не изменилось,
  `Reported` остаётся прежним; если устройство пришлёт report — он и будет
  новым state.

### 3.4 Subscription

```text
subscriber: filter + try_push(event, ctx) + wake(ctx)
Dispatcher: wait → прочитать Journal → фильтр → try_push → wake → continue
```

- `try_push` только копирует compact-факт в inbox подписчика;
- `false` означает «inbox полон» — это локальная потеря подписчика;
- `wake` только будит задачу; обработка идёт в задаче подписчика;
- Dispatcher **не** вызывает `Domain::get`, не пересобирает UI, не пишет state;
- Dispatcher не держит Domain-lock; `try_push/wake` не могут вызывать Domain API;
- медленный подписчик задерживает только свою доставку.

## 4. Domain

### 4.1 Entity Store

- Все entity равны; таблица выбирается по opaque `entity_type`.
- Ключи и записи — фиксированного размера; никаких указателей и вложенного heap.
- `put` возвращает `changed`; неизменённая запись не создаёт факт.
- `get`/`iter` — copy-out в буфер вызывающего; borrow не сохраняется.
- Состояние и факт пишутся в одном сериализованном порядке.

### 4.2 Journal

- Bounded ring в RAM, короткая история; **не Event Sourcing**.
- Компактный `Fact`; большие данные — только transient payload.
- Append не блокирует producer; при заполнении вытесняется oldest.
- `COMMAND_SENT` — audit, а не канал доставки команды.
- History replay при старте не выполняется.
- Gap фиксируется счётчиком/диагностикой, а не скрывается.

### 4.3 Dispatcher

- Отдельная задача/поток-потребитель Journal.
- Свой cursor и свой dispatch-lock; **не** Domain-lock.
- Фильтр по `kind/source/entity`.
- Никакой бизнес-логики, никаких вызовов подписчика «внутри» доставки.
- При `cursor < oldest` фиксирует gap, переходит к oldest и продолжает.

### 4.4 Commands

- Fire-and-forget: Domain маршрутизирует command executor'у и возвращается.
- Executor обязан быть коротким: скопировать args и передать в очередь adapter'а.
- Command не меняет state и не создаёт ожидающую задачу.
- Результат транспорта не является подтверждением state.

## 5. Подписчики

### 5.1 Display

- Подписан на `PROPERTY_STATE`, `EVENT`, `GROUP`, `GROUP_ITEM`,
  `CAPABILITIES`, `ENTITY`, `AUTOMATION`, `WEATHER`, `LOCATION`, `SETTINGS`,
  `WIFI_*` по своим фильтрам.
- Начальный экран строится разовым чтением Domain; history не нужна.
- `PROPERTY_STATE` обновляет конкретный виджет `(entity, property)`,
  а не пересобирает весь экран.
- Пересборка экрана — только при изменении состава/имени/capabilities.
- Событие — trigger; если нужен state, подписчик читает его из Entity Store.
- Display не опрашивает Domain каждый тик.

### 5.2 Web

- Тупой транспорт canonical records: snapshot + дельты.
- Подписан на те же типы фактов, что и Display; не хранит source of truth.
- Для каждого клиента — собственная TX-очередь; запись в сокет **никогда**
  не выполняется под Domain-lock.
- Медленный/зависший клиент не блокирует Domain и Zigbee; переполнение его
  очереди — kill session, а не stall всей системы.
- Web не меняет state сам: команды идут через Domain, state — только report.

### 5.3 Automation

- Подписан на `EVENT`/`STATE` canonical-типов, не на source.
- Держит производный RAM-индекс триггеров; source of truth — правила в Domain.
- При срабатывании постит команду, не вызывает Zigbee напрямую.
- Не ждёт результата команды; следующий state придёт report'ом.
- Не исполняется в контексте Dispatcher'а.

### 5.4 System / Wi-Fi

- System публикует canonical state/event напрямую, без ZCL.
- Wi-Fi публикует scan/known/status как обычные сущности.
- Их обновления проходят тот же Domain/Dispatcher путь.

## 6. Команды и UI

- Кнопка/переключатель в UI создаёт **intent**, а не новое состояние.
- Отправка команды не включает optimistic `PROPERTY_STATE`.
- UI может показать локальный pending-индикатор, но он не является state.
- Если устройство не прислало report, UI продолжает показывать последний
  подтверждённый `Reported`; таймаут команды не придумывает новый state.
- UI не ждёт ACK/Default Response, чтобы продолжить работу.
- CommandCore, OperationId и retry, если нужны, живут в adapter'е и не
  протекают в canonical Domain API.

## 7. Хранилище и flash

### 7.1 RAM

- `PROPERTY_STATE` — **RAM**, не flash.
- Journal — RAM ring.
- Transient payload — RAM ring.
- Entity/Capabilities/Logical Device — RAM, восстанавливаются интервью,
  если не принято отдельное решение об их кэше.
- Runtime-индексы Automation/Display — RAM, производные.

### 7.2 Flash

Persistent config только:

- Logical Device / device registry (adapter-private);
- group / group_item / group_automation;
- automation rules;
- settings, location, known Wi-Fi;
- при необходимости — entity/capabilities как кэш, но **не state**.

State write-through во flash запрещён: `mstore` RAM|FLASH даёт p50 ~350 µs,
p99 ~127 ms и compaction до сотен мс — это недопустимо в hot path report'ов.

### 7.3 Восстановление

- После reboot state пуст; устройства присылают его заново.
- UI не ждёт «исторического» state; показывает только подтверждённые report'ы.
- Persistent config читается из mstore; state — нет.

## 8. Ёмкости и бюджет

V2 был **fixed-capacity by design**; это не «безлимит», а предсказуемая память.
Целевой V3 использует V2-размеры как стартовый бюджет, а не случайные 64/16.

| Таблица | Целевая ёмкость | Backing |
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

- 256 state-слотов — примерно 32 устройства × 8 свойств.
- Если целевой парк больше 32 устройств, state-таблица становится
  динамической/большей по измеренному бюджету; нельзя молча терять state.
- Все ёмкости собраны в composition root, а не размазаны по сервисам.
- Переполнение state — явная ошибка/диагностика; state не вытесняется
  случайным образом.

## 9. Потоки и блокировки

```text
Adapter task:   report/command → Domain mutation → Journal append
Dispatcher:     Journal → filter → try_push/wake
Subscriber:     inbox → прочитать state/payload → своя работа
```

- Domain-lock не удерживается во время `try_push`, `wake`, UI, сокета,
  flash-write или Zigbee-отправки.
- Adapter не вызывает display/web напрямую.
- Dispatcher не вызывает subscriber body.
- Subscriber не вызывает мутирующий Domain API из `try_push`.
- Долгие операции (flash, radio, WS) — в своей задаче/очереди.
- Один producer факта не должен ждать consumer'а.

## 10. Ошибки и перегрузка

- State: `NoSpace`/`InvalidSize` — явная ошибка; не молчаливая потеря.
- Journal: вытеснение oldest допустимо; gap считается и логируется.
- Subscriber inbox: `false` — локальная потеря только этого подписчика;
  счётчик обязателен; следующий state можно перечитать.
- Command queue: отказ возвращается источнику; state не меняется.
- Web: медленный клиент убивается по таймауту; Domain не блокируется.
- Логи в hot path — rate-limit/debug; INFO только на реальные изменения
  и события.
- Любая потеря наблюдаема: счётчик, а не «само пройдёт».

## 11. Границы модулей V3

| Слой | Ответственность |
|---|---|
| `ha-domain` | Entity Store, Journal, Dispatcher, Commands, subscriptions |
| `ha-model`/canonical | Entity/Device/Property/Action/Event/Capability, точные ABI-формы |
| `zigbee-devices` | interview, registry, bindings, ZCL↔canonical, CommandRouter |
| `z2m-rs` | profiles/fingerprints/converters/configure; не знает Domain/UI |
| `zigbee-exec` | транспортная надёжность запросов (TSN/retry/fake) |
| `ha-automation` | подписчик фактов + RAM-индекс правил |
| `ha-web` | canonical wire protocol; WebSocket framing |
| `ha-app` | P4 bootstrap, C-мост, задачи/стеки, composition |
| `micro-db` | storage primitives; не знает Domain/Zigbee |

## 11.1. Rust-перевод V2: что упрощается

| V2 (C) | V3 target (Rust) | Что сохраняется |
|---|---|---|
| `domain_entity_t` + runtime registry | `enum EntityType` + типизированный `DomainTables` | те же числовые canonical ids и формы |
| `domain_entity_desc_t`, `void*`, размеры в runtime | `trait Record { const KEY_LEN; const RECORD_LEN; }`, `#[repr(C)]`, const asserts | точный ABI, copy-in/copy-out |
| `mstore Table/Ring` с `void*` | `Table<K, V, const N: usize>`, `Ring<Fact, N>` | fixed capacity, generation/version, byte equality |
| `domain_subscription_desc_t` + `try_push` + `wake` | `SyncSender<Fact>` в задаче подписчика; `try_send` | inbox принадлежит подписчику, локальная потеря |
| callback-команды + `ctx` | `Command`/`CommandSink` trait или `match` | fire-and-forget, без Domain-ожидания |
| `sys_error_t` | `enum Error` + `Result`; маппинг на C/WS-границе | одинаковый смысл ошибок |
| `char[N]` + ручная нормализация | `[u8; N]` + `Copy`/`Default`, явные `reserved` | canonical layout; zero-fill |
| `domain_journal_*` + dispatcher lock | `Mutex<Domain>` только на чтение Journal/копирование факта; отправка вне lock | producer не ждёт subscriber'а |
| `domain_entity_meta` | `changed`/`version` из таблицы | дешёвая проверка изменения |
| static arrays + `static_assert` | `const` capacity + compile-time layout tests | предсказуемая RAM |
| C host tests | Rust `#[test]` на тех же golden-векторах | V2 parity |

Упрощения не меняют поведение:

- **`mpsc::SyncSender`** заменяет самодельный inbox/wake: `try_send` — та же
  локальная потеря, `Receiver` живёт в задаче подписчика.
- **`#[derive(Copy, Clone, Default)]`** убирает ручной `memset`/`memcpy`.
- **Типизированный `StateKey`** убирает `void*` и runtime-проверки размера ключа.
- **`match` по `EntityType`/`Command`** убирает таблицу callback'ов и `ctx`.
- **`Result`** убирает ручные коды ошибок внутри Rust; наружу — тот же код/смысл.
- **RAII** убирает `deinit`/double-free/`release`-ошибки.

Что **не** упрощаем и не переизобретаем:

- state не становится event-sourced;
- state не пишется во flash;
- команда не ждёт и не создаёт state;
- Dispatcher не исполняет подписчика;
- state не проходит через глобальную очередь;
- canonical ABI не меняется;
- capacity не становится «магически безлимитной»;
- ошибки/потери не становятся молчаливыми.

## 12. Что в текущем V3 нарушает target

Исторические нарушения и текущий статус:

- **Сделано:** `PROPERTY_STATE` — RAM; `write_state`/`remove_states` не
  используют persistence. `restore_domain` state не восстанавливает и не
  чистит flash.
- **Сделано:** `bridge::STATES`/`EVENTS` удалены. `frames::IngestSink` пишет
  report/event в Domain напрямую; остался только bounded capability mailbox
  (`capabilities.rs`) для configure.
- **Сделано:** capacity приведены к V2-бюджету; journal 64.
- **Сделано:** state/event/capabilities/entity идут через Domain-подписки
  (`SyncSender<Fact>`); Zigbee register/caps/state не вызывает Display/Web
  напрямую. Display/Web/Automation разбирают факты в своих drain-функциях.
- **Сделано:** non-Zigbee config (system/wifi/settings/rename) больше не
  обновляет Display/Web прямыми вызовами; status/настройки идут через
  Domain-факты.
- **Сделано:** `display::drain` коалесцирует composition-facts в один rebuild
  за цикл.
- **Осталось:** `intake=8`, `capabilities=32` — проверить и заменить на
  V2-размеры/явные counters.
- **Осталось:** V2-фикс web (per-client TX queue/ACK, не держать Domain-lock
  на сокете) пока не воспроизведён.

Эти пункты удаляются/заменяются, а не обрастают новыми обходами.

## 13. План миграции

### Этап 1 — truth и бюджет

- `PROPERTY_STATE` → RAM; убрать persist state и restore state.
- Поднять ёмкости до V2-бюджета (state 256, entity/device/caps 32,
  group_item 128, journal 64).
- Явные счётчики переполнения; без изменения UI-логики.

### Этап 2 — убрать bridge

- Decode → Domain напрямую; `ENTITY_UPSERTED`/`EVENT` → Journal.
- Удалить `bridge::STATES`/`EVENTS`.
- Dispatcher читает Journal и доставляет подписчикам.

### Этап 3 — подписчики

- Display/Web/Automation регистрируют inbox + task.
- Убрать прямые `display::on_state`/`web::upsert` из main/adapter.
- Display обновляет виджет по `(entity, property)`, не весь экран.
- Web per-client TX queue без per-frame ACK; медленный клиент закрывается, Domain-lock не держим.

### Этап 4 — нагрузка

- 32 устройства, 8 свойств, отчёты 5–60 с.
- Проверить: ни один report не потерян до Domain; медленный Display/Web
  не останавливает producer; команда не меняет state до report; flash не
  пишется на state; gap/drop counters видны.
- Замерить heap, stack high-water, latency, журнал/очереди.

## 14. Критерии готовности

- Report → state виден в Domain без промежуточной state-очереди.
- `changed=false` не создаёт факт и не будит подписчиков.
- Command не создаёт state и не создаёт ожидающую задачу.
- UI/Display обновляются только по подтверждённому state или event.
- Медленный подписчик не блокирует Domain, Dispatcher и Zigbee.
- State не пишется во flash; после reboot state приходит report'ами.
- Переполнения state/journal/subscriber/command наблюдаемы.
- 32 устройства в нагрузочном стенде не дают silent loss state.

## 15. Открытые решения

1. Целевой парк: 32 устройства или больше (от этого зависит state capacity).
2. Динамическая state-таблица vs фиксированный бюджет V2.
3. Persist entity/capabilities как кэш или восстанавливать интервью.
4. Точные размеры inbox Display/Web/Automation.
5. Web TX queue: V2 final не использует per-frame ACK; подтвердить схему.
6. Политика report interval для C6 (5 с / 30 с / change-driven).
7. Нужна ли история Event для UI (сейчас — bounded Journal, без replay).
