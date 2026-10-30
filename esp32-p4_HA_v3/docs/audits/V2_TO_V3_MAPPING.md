# V2 → V3: аудит и таблица соответствия

> Статус: **аудит текущей V3-реализации относительно архитектурного эталона V2**.
>
> Эталон: `esp32-p4_HA_v2/docs/ARCHITECTURE.md` и контракты в
> `esp32-p4_HA_v2/docs/domain/`, `services/`, `clients/`, `storage/`.
>
> Целевой контракт V3: [V3_LAYERS.md](../architecture/V3_LAYERS.md).
> План реализации: [V3_IMPLEMENTATION_PLAN.md](../plan/V3_IMPLEMENTATION_PLAN.md).
>
> Этот документ не является планом рефакторинга. Он фиксирует, что в V3 уже
> соответствует V2, что перенесено неверно, чего нет и что является обходом.

## 1. Статусы

| Статус | Значение |
|---|---|
| `KEEP` | реализация соответствует V2-контракту; допустимы только Rust-адаптации |
| `PORT` | функция есть, но перенесена не полностью или с другим API |
| `REWORK` | функция работает, но владение/граница/поток нарушает V2 |
| `MISSING` | V2-слой/механизм отсутствует |
| `DELETE` | архитектурный обход; должен быть удалён после переноса замены |

## 2. Слои и контракты

| V2 компонент | V2-контракт | Текущий V3 | Статус | Действие |
|---|---|---|---|---|
| `ha_model` | canonical vocabulary/ABI, не зависит от Domain | `crates/ha-model`; `ha-domain` реэкспортирует | **PORT** | ABI отделён; Domain его не содержит |
| `ha_model` record layout | fixed ABI, zero-fill, explicit padding, stable keys | canonical records + Rust tests | **KEEP** | Сохранить layout/тесты; не менять ABI |
| `mstore` | Table/Ring, slot/generation/version, RAM/flash | `crates/micro-db` (`Table`, `Ring`, `PersistedTable`) | **KEEP** | Сохранить концепцию; проверить, что storage-типы не протекают наружу |
| `Domain` Entity Store | canonical state, changed=false, copy-in/out | `ha-domain` + `zigbee-devices::DomainAdapter` регистрирует таблицы | **REWORK** | Domain должен владеть lifecycle записи; adapter/persistence не должны подменять Domain |
| `Domain` Journal | compact facts, bounded ring, oldest eviction, gap | `ha-domain` Ring + `Fact` | **KEEP** | Сохранить; добавить task/wake в Dispatcher |
| `Domain` Transient Payload | best-effort payload ring | `ha-domain` payload ring | **KEEP** | Сохранить |
| `Domain` Dispatcher | отдельная задача, cursor, filter, try_push/wake | есть `DispatchHandle`/signal и отдельная dispatcher-задача; main больше не вызывает `dispatch_once` | **PORT** | Довести lock/ownership и проверить gap/drop под нагрузкой |
| `Domain` Commands | `domain_post` → executor, `COMMAND_SENT`, без pending | `ha-domain` registry + `zigbee_service`; `CommandCore` adapter-private | **PORT** | Единый `Domain.post`; pending/desired adapter-private |
| `Device service` | neutral lifecycle/ownership, register/bind/rename/remove, remover registry | `crates/ha-device` | **PORT** | Zigbee регистрирует remover |
| Zigbee adapter | private registry/interview/bindings/ZCL/radio, canonical наружу | `zigbee-devices` + `native_zigbee` + `zigbee_service` | **PORT** | executor/loop больше не в `main` |
| Zigbee decoder/projection | ZCL↔canonical, profiles/converters/configure | `zigbee-devices`, `z2m-rs` | **KEEP** | Сохранить; это сильная часть V3 |
| Automation service | rules in Domain, subscriber, command через Domain | `ha-automation` + `ha-app/automation_task.rs`; task/inbox | **PORT** | Команды идут через `Domain.post` |
| Web protocol | canonical records verbatim, snapshot/delta | `ha-web` + `web-ui` | **KEEP** | Сохранить wire-формат/UI |
| Web service | per-client TX queue, no ACK, no source of truth | `ha-app/src/web.rs`; web-drain + queue | **PORT** | model остаётся cache |
| Display | LVGL task, subscription, widget по `(entity,property)`, groups | `ha-app/src/display.rs`; display-drain task вынесен | **PORT** | Довести независимость LVGL task и проверить на нагрузке |
| System | SNTP/GeoIP/weather → Domain state/event | `ha-app/src/system.rs` | **REWORK** | Публиковать всё через Domain; убрать direct automation call |
| Wi-Fi | owner радио, scan/known/status Domain entities, known flash | `native_net/ha_wifi.c` + `ha-app/wifi.rs` | **PORT** | Отдельная задача; known в Domain/mstore |
| Wi-Fi known AP | Domain entity, flash | `WIFI_KNOWN` + mstore; C RAM list | **PORT** | NVS больше не app storage |
| Composition root | регистрация descriptors/capacities, запуск задач | `ha-app/src/main.rs` | **PORT** | Только сборка и intake CRUD |
| Persistent policy | state RAM; config flash; Zigbee network `zb_storage` | `store.rs`, `domain_adapter.rs`, `partitions.csv` | **PORT** | Сохранить; проверить, что Domain, а не adapter, владеет canonical config lifecycle |
| Error model | `sys_error_t`, один код на слой | Rust `enum Error` (micro-db/domain/adapter) | **PORT** | Сохранить смысл кодов; не смешивать Domain и adapter ошибки |
| Host tests | V2 layer tests | Rust tests в crates | **PORT/KEEP** | Достроить тесты на runtime ownership, drops, Domain API |

## 3. Что сохранить

Это уже соответствует V2 по смыслу и должно остаться:

- fixed record layouts и canonical ids (`ha-domain/src/canonical.rs`);
- `micro-db` Table/Ring и тесты;
- `z2m-rs` profiles/fingerprints/converters/configure и golden tests;
- `zigbee-devices` projection/interview/profile/registry/descriptor;
- Zigbee C bridge и RCP-схема;
- `ha-automation` rule engine;
- `ha-web` framing + web-ui;
- display LVGL UI (структура экранов, groups/items, widgets);
- C6 RCP и `zb_storage` разделение.

Эти части не нужно переписывать «под новую архитектуру». Их нужно встроить в V2-границы.

## 4. Что переработать

### 4.1. Domain runtime

Сейчас:

- `main` вызывает `domain.dispatch_once()`;
- Display/Web/Automation дренируются в `main`;
- Domain API используется из одного цикла, а не из задач;
- `Domain` содержит canonical-модель и остатки command routing.

Нужно:

- Domain API — потокобезопасный фасад V2;
- Dispatcher — отдельная задача с cursor/wake;
- subscriber bodies — в задачах подписчиков;
- main не дренирует подписчиков.

### 4.2. Command path

Сейчас:

- UI/Display идут через `intake` → `Domain.post`; Automation постит напрямую;
- Domain command executor вызывает `zigbee_service::State::submit`;
- `CommandCore`, executor и bindings принадлежат Zigbee service;
- `COMMAND_SENT` пишется Domain после успешной передачи.

Нужно:

- `Domain.post(command, target)` — единственный вход intent;
- executor adapter'а короткий, fire-and-forget;
- pending/desired/retry — adapter-private;
- state меняется только report'ом.

### 4.3. Device service

Сейчас lifecycle/ownership размазан между `DomainAdapter`, `main` и Zigbee
соглашениями.

Нужно:

- нейтральный Device service;
- canonical register/bind/rename/remove;
- remover registry для adapter;
- Zigbee adapter не становится владельцем canonical lifecycle.

### 4.4. Web/Display

Сейчас:

- web service и display client — функции в `ha-app`, вызываемые из main;
- модель web — кэш, но нет per-client bounded queue;
- Display обновляется из main.

Нужно:

- web service task + per-client TX queue;
- Display task/inbox + LVGL updates;
- ни один из них не source of truth;
- медленный клиент/дисплей не тормозит Domain/Zigbee.

### 4.5. Wi-Fi

После переноса known AP/status/scan в Domain; Wi-Fi task владеет C-радио.

Нужно:

- Wi-Fi service task;
- `WIFI_SCAN/WIFI_KNOWN/WIFI_STATUS` — Domain entities;
- known — flash;
- команды scan/connect через Domain;
- default NVS не является app storage.

## 5. Что удалить после переноса замены

- `crates/ha-app/src/bridge.rs` — уже удалён.
- `capabilities.rs` mailbox как обход Domain capability flow — заменить Domain-путём.
- Прямые `display::set_wifi`, `web::upsert` из bootstrap/restore — убрать после
  появления Domain/subscriber пути.
- Прямой `intake → executor` для команд, если он обходит `Domain.post`.
- `ha-domain` command registry, если V2-путь команд реализован через Domain.post
  и adapter executor.
- Любые дубли state/model, которые вводят второй source of truth.
- `web::MODEL` как source of truth; допустим только как transport cache.

## 6. Что отсутствует

| V2-компонент | Статус | Комментарий |
|---|---|---|
| Отдельный Device service | **PORT** | crates/ha-device: register/bind/rename/remove + remover registry |
| Отдельная Dispatcher task | **PARTIAL** | Signal/task есть; Domain остаётся под общим Mutex |
| Subscriber tasks для Web/Display/Automation | **PORT** | Все три вынесены; Domain общий Mutex |
| Web per-client TX queue | **PORT** | Bounded per-client queue; медленный клиент закрывается |
| Wi-Fi Domain entities | **PORT** | status/scan/known/commands в Domain; C только радио |
| `ha-model` crate | **PORT** | canonical ABI вынесен в `crates/ha-model` |
| V2 `sys_error_t` parity | **PARTIAL** | Rust enum есть, но границы ошибок надо проверить |
| 32-device load evidence | **MISSING** | Стенд не проведён |

## 7. Главные нарушения V2 в текущем коде

1. `main` — только composition root; Zigbee executor/loop и subscriber drains вынесены.
2. `Domain` не является единственным runtime-владельцем canonical state:
   canonical-модель, command registry и persistence-политика размазаны.
3. Device service есть, но adapter всё ещё создаёт Entity/capabilities и передаёт их на bind.
4. Domain обёрнут в DomainHandle; main-loop берёт короткие scoped locks, а не держит один на итерацию.
5. Property command идёт `Domain.post` → callback Zigbee service → executor; executor вне `main`.
6. Wi-Fi status/scan/known уже Domain; C хранит только радио-состояние.
7. `ha-model` отделён; `ha-domain` реэкспортирует canonical ABI.

Это не ошибки отдельных файлов. Это отсутствие V2-владения.

## 8. Следующий аудит

Перед кодом нужно:

1. Сверить каждый пункт этого документа с V2-контрактом.
2. Для `REWORK/MISSING` определить точный V2-контракт и Rust-адаптацию.
3. Зафиксировать порядок переноса в `V3_LAYERS.md`.
4. Только после этого менять код.

Код в рамках этого аудита не менялся.
## 9. Статус первого переноса runtime

Сделано после аудита:

- Domain получил `DispatchHandle`/wake signal.
- Dispatcher вынесен в отдельную задачу; `main` больше не вызывает `dispatch_once`.
- Web-drain и Display-drain вынесены в отдельные задачи; `main` больше не вызывает
  `web::drain` / `display::drain` / `display::tick`.
- Domain разделяется через `Arc<Mutex<Domain>>`; producer/Dispatcher/subscriber
  сериализуются на Domain-lock.
- Automation вынесен в task; команды публикуются через `Domain.post`.
- Property command идёт через `Domain.post`: web/display intake → Domain command executor →
  `zigbee_service` → executor queue. Источники (`UI=2`, `Automation=3`, `System=4`, `WiFi=5`)
  и `COMMAND_PROPERTY=6` совпадают с V2.
- Zigbee executor, CommandCore, bindings, interview/configure и radio loop вынесены в
  `zigbee_service`; `main` не владеет Zigbee-состоянием.
- Device service вынесен в `ha-device`: register/bind/rename/remove и remover registry.
- Web получил bounded per-client TX queue; медленный клиент закрывается, Domain не блокируется.
- Wi-Fi service task владеет C-радио, публикует `WIFI_STATUS/WIFI_SCAN/WIFI_KNOWN`, принимает
  scan/connect команды через `Domain.post`; known AP персистентны в Domain/mstore.
- `ha-model` вынес canonical ABI; `ha-domain` реэкспортирует его.
- Runtime использует `DomainHandle` (`with`/`with_mut`), а не голый `Arc<Mutex<Domain>>` в задачах.
- `main` больше не держит Domain lock на всю итерацию: только короткие scoped locks.

Ещё не сделано:

- 32-device нагрузочный стенд не проведён: на стенде 3 устройства.

## 10. План реализации

Рабочий план вынесен в [V3_IMPLEMENTATION_PLAN.md](../plan/V3_IMPLEMENTATION_PLAN.md).

Порядок:

1. Domain API + persistence ownership.
2. Zigbee adapter cleanup.
3. System / Automation / Settings / Display ownership.
4. Runtime и intake.
5. `main.rs` как composition root.
6. V2 scenario verification.

До завершения этапа 1 не менять `main.rs` и `DomainAdapter`.