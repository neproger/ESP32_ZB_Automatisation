# HA V2 → Rust: план портирования

> Статус: **единственный активный план работ**.
> Главный принцип: [V2_TO_RUST_PORTING_PRINCIPLES.md](V2_TO_RUST_PORTING_PRINCIPLES.md).
>
> Источник истины — фактическая реализация и проверенное поведение
> `esp32-p4_HA_v2`, а не текущий Rust-код и не черновики новой архитектуры.
> При расхождении документации V2 с кодом V2 приоритет у кода V2; расхождение
> фиксируется в отчёте текущей задачи.
>
> Исключения: Zigbee — текущая архитектура V2+Z2M (§5.2); полезные уже
> реализованные Rust-улучшения — после оценки (§5.3).

## 1. Цель

Перенести проверенную архитектуру Home Assistant V2 на Rust, сохранив
ответственность слоёв, API-контракты по смыслу, алгоритмы, последовательность
взаимодействия и поведение при штатных и предусмотренных V2 ошибках.

Rust меняет реализацию и типы, но не контракт. `micro-db`, `ha-domain` и уже
готовые Rust-компоненты используются как есть; параллельные механизмы не
создаются.

## 2. Исходное состояние

### 2.1. V2

V2 содержит проверенные на P4 слои:

```text
ha_model   canonical vocabulary / fixed ABI
sys        error type and layer codes
mstore     Table / Ring / flash regions
domain     Entity Store + Journal + Payload + Dispatcher + Commands
device     neutral Logical Device / Entity lifecycle
zigbee     transport adapter, private registry, ZCL bridge
automation rule subscriber and command producer
web        HTTP/WS canonical transport
display    LVGL client
system     SNTP / GeoIP / weather
wifi       external C3 radio owner
main       composition root
```

Проверенные контракты и исходники:

- `esp32-p4_HA_v2/docs/ARCHITECTURE.md`
- `esp32-p4_HA_v2/docs/RECORD_MODEL.md`
- `esp32-p4_HA_v2/docs/domain/*.md`
- `esp32-p4_HA_v2/docs/services/*.md`
- `esp32-p4_HA_v2/docs/clients/DISPLAY.md`
- `esp32-p4_HA_v2/docs/storage/*.md`
- `esp32-p4_HA_v2/main/app_main.c` — фактические capacities, backing и порядок
  запуска.

### 2.2. Rust-компоненты

Готовые или в основном готовые компоненты:

- `crates/ha-model` — canonical ABI и fixed records;
- `crates/micro-db` — Table, Ring, Flash, RegionManager, RAM/Flash backends;
- `crates/ha-domain` — Entity Store, Journal, payload ring, dispatcher,
  commands, restore/maintenance API;
- `crates/ha-device` — lifecycle/ownership API;
- `crates/ha-automation` — rule engine;
- `crates/ha-web` — canonical wire framing;
- `crates/zigbee-devices`, `crates/z2m-rs`, `crates/zigbee-exec`,
  `crates/zigbee-frames` — Zigbee adapter;
- `crates/ha-app` — composition root, C bridges и текущие сервисные задачи.

### 2.3. Фактическое состояние портирования

| Слой V2 | Rust | Состояние |
|---|---|---|
| `ha_model` | `ha-model` | ABI готов; есть consumer-side дублирование offsets |
| `mstore` | `micro-db` | примитивы готовы и покрыты host-тестами |
| `domain` | `ha-domain` | core готов; production persistence/restore не подключены |
| `device` | `ha-device` + `zigbee_service` | API есть; adapter всё ещё создаёт Entity/Capabilities |
| `zigbee` | `zigbee-*` + `ha-app` | особый слой V2 + Z2M; текущая архитектура сохраняется; закрываются gaps относительно собственной цели |
| `automation` | `ha-automation` + `automation_task` | engine есть; task/inbox и ABI-парсинг требуют доводки |
| `web` | `ha-web` + `ha-app/web.rs` | protocol готов; service использует кэш и не имеет V2 snapshot semantics |
| `display` | `ha-app/display.rs` | subscriber есть; Wi-Fi читается в обход Domain |
| `system` | `ha-app/system.rs` + `main.rs` | fetch есть; публикация/события частично в main |
| `wifi` | `ha-app/wifi.rs` + `native_net` | радио и Domain entities есть; Display обходит Domain |
| `main` | `ha-app/main.rs` | остаётся оркестратором, а не только composition root |

### 2.4. Известные расхождения, которые нельзя закреплять как новую архитектуру

1. **Persistence split.** `store.rs` ведёт отдельную generic-таблицу и
   `restore_domain`, а `Domain::new_with_storage`/`restore` не используются.
   V2 Domain — единственный writer canonical storage.
2. **Manifest backing.** Текущий `domain_tables` делает persistent
   `Logical Device`, `Entity`, `Capabilities`; V2 `app_main.c` держит их в RAM,
   а persistent config — только group/group_item/group_automation/automation/
   location/settings/wifi_known. Это нужно привести к V2, а не документировать
   как новое решение.
3. **Main orchestration.** `main` всё ещё вызывает `intake`, `system::take`,
   `display::take_nav`, `wifi::take_scan`, `capabilities::take`.
4. **Canonical lifecycle.** Zigbee adapter создаёт Entity/Capabilities и
   передаёт их Device service, тогда как V2 lifecycle принадлежит Device service.
5. **Canonical parsing.** `ha-automation`, `ha-web`, display и main местами
   разбирают fixed records вручную вместо `ha-model` структур.
6. **Источники фактов.** Часть System/UI-записей помечается `SOURCE_ZIGBEE`.
7. **Zigbee functional gaps.** `Binding.confirmed` не обновляется после
   configure; report любого значения снимает desired state; transport bridge
   поддерживает не все стандартные ZCL команды. Это дефекты текущей
   Zigbee-реализации относительно её собственной цели V2+Z2M, а не требование
   механически копировать Zigbee-слой V2.

### 2.5. Данные старого формата

В этом плане **нет миграции данных между форматами**. Если на конкретном
устройстве есть пользовательские данные, которые обязательно нужно сохранить
при смене storage layout, это отдельное решение и отдельная задача. Оно не
определяет архитектуру портирования и не должно добавлять migration markers,
recovery state machine или новые гарантии в Domain.

## 3. Архитектурное соответствие

| Слой V2 | Ответственность | Rust-компонент | Состояние | Следующий шаг |
|---|---|---|---|---|
| `ha_model` | canonical vocabulary, fixed ABI, identity | `ha-model` | готов | заменить ручные offsets в потребителях |
| `sys` | единый код ошибки/слоя | Rust `Error` enums | частично | сохранить смысл кодов на границах |
| `mstore` | Table/Ring/Flash/Region | `micro-db` | примитивы готовы | не менять без необходимости |
| `domain` | Entity Store, Journal, Payload, Dispatcher, Commands | `ha-domain` | core готов; persistence нет | подключить production storage/restore |
| `device` | Logical Device/Entity lifecycle | `ha-device` | частично | перенести lifecycle из adapter |
| `zigbee` | transport, private registry, ZCL bridge | `zigbee-*`, `ha-app` | **особый слой V2+Z2M** | сохранить текущую архитектуру; закрыть functional gaps без приведения к V2 |
| `automation` | rules subscriber → commands | `ha-automation`, task | частично | убрать дубли ABI и довести task |
| `web` | canonical snapshot/delta transport | `ha-web`, `ha-app/web.rs` | protocol готов | привести service к V2 |
| `display` | LVGL client | `ha-app/display.rs` | частично | убрать обход Domain |
| `system` | SNTP/GeoIP/weather | `ha-app/system.rs` | частично | публиковать из сервиса |
| `wifi` | radio owner + Domain entities | `ha-app/wifi.rs`, C bridge | частично | Display/System только через Domain |
| `main` | composition root | `ha-app/main.rs` | частично | убрать бизнес-логику и polling |

## 4. Последовательность портирования

Последовательность определена зависимостями V2: Domain должен владеть
canonical state до Device/service интеграции; Device service — до Zigbee
lifecycle; Wi-Fi — до System; Display запускается после начального чтения
Domain.

### Этап 1 — Domain: persistence и restore

> Статус: **реализовано и проверено на P4 частично**. Commit/push не
> выполнялись. Workspace tests, `build-ha-app` и `check-domain-api` зелёные;
> hardware validation: [STAGE1_HARDWARE_VALIDATION.md](../audits/STAGE1_HARDWARE_VALIDATION.md).
> Уникальный persistence test через UI/Web ещё не выполнен.

**Исходный слой V2:** `domain` + регистрация таблиц в `main/app_main.c`.

**Назначение:** Domain — единственный writer canonical state и persistent
config; restore читает canonical tables, не публикует факты.

**Зависимости:** `micro-db`, `ha-model`, `ha-domain` storage seams.

**Что сделать:**

- привести descriptor manifest к V2 backing/capacity из `app_main.c`;
- использовать production `DomainStorageProvider`, `Domain::new_with_storage`,
  `Domain::restore`;
- `DomainMaintenance` не подключать в Stage 1: V2 не требует startup/periodic
  compaction; оставить существующий API без расширения;
- убрать параллельный `store::save_domain`/`restore_domain`/`DOMAIN_DB` после
  замены пути;
- сохранить V2 semantics: state — RAM, persistent config — Domain/mstore;
- не добавлять migration/recovery механизмы.

**Проверки:** restore после reboot, отсутствие facts при restore, persistent
RAM/flash policy, NoSpace/ошибки storage, host-тесты на SimFlash.

**Критерий завершения:** production app использует Domain-owned persistence;
в коде нет второго canonical storage path.

**Предусловие:** если на устройстве есть данные, которые нельзя потерять,
остановиться до отдельного решения о данных. Не проектировать migration в
рамках этого этапа.

### Этап 2 — Device service

**Исходный слой V2:** `device`.

**Назначение:** transport-neutral Logical Device/Entity ownership.

**Зависимости:** Domain, `ha-model`.

**Что сделать:**

- перенести register/bind/rename/remove и создание Entity/Capabilities из
  Zigbee adapter в Device service;
- сохранить V2 cascade removal и remover registry;
- adapter регистрирует remover, но не владеет canonical lifecycle.

**Проверки:** register/idempotency, bind ownership, rename, remove cascade,
adapter remover failure, host tests.

**Критерий завершения:** adapter не создаёт canonical Entity/Capabilities и не
выполняет canonical cleanup.

### Этап 3 — Zigbee adapter

**Исходная база:** текущий Zigbee-слой на основе V2 + Z2M.
Это единственное предусмотренное архитектурное исключение.

**Назначение:** единственный владелец ZCL/транспорта и Zigbee-private данных.

**Зависимости:** Domain, Device service, `ha-model`, `z2m-rs`, `zigbee-exec`.

**Что сделать:**

- сохранить текущую целевую архитектуру Zigbee и полезные механизмы Z2M;
- не приводить interview/endpoint/capabilities к V2 механически;
- закрыть functional gaps текущей реализации: bindings после configure,
  report/desired semantics, поддерживаемые ZCL команды;
- сохранить adapter-private flash для Zigbee network/registry;
- не менять canonical ABI и не переносить canonical lifecycle в adapter.

**Проверки:** host lifecycle/parity tests, command validation, report→state,
interview, remove/leave; затем P4 scenarios.

**Критерий завершения:** Zigbee реализует свою целевую V2+Z2M модель, соблюдает
общие контракты Domain/Device и не выходит за canonical границу.

### Этап 4 — Automation

**Исходный слой V2:** `automation`.

**Назначение:** подписчик canonical facts, rule engine, command producer.

**Зависимости:** Domain, `ha-model`.

**Что сделать:**

- использовать canonical `ha_rule_t`/record layout, убрать ручные offsets;
- сохранить task/inbox, trigger/condition/action semantics и loop guard;
- команды только через Domain; не добавлять новые режимы, которых нет в V2.

**Проверки:** event/state/time triggers, conditions, actions, edge detection,
inbox loss, command path.

**Критерий завершения:** automation behavior и ownership совпадают с V2.

### Этап 5 — Wi-Fi и System

**Исходные слои V2:** `wifi`, `system`.

**Назначение:** Wi-Fi владеет радио; System публикует canonical time/location/
weather state/events.

**Зависимости:** Domain, `ha-model`; System зависит от Wi-Fi.

**Что сделать:**

- Wi-Fi status/scan/known и commands — только через Domain;
- System публикует state/events из своей задачи, с правильным source;
- убрать прямой доступ Display к C Wi-Fi status;
- сохранить V2 reconnect/backoff/autoconnect behavior.

**Проверки:** scan/known/status, connect/backoff, SNTP/GeoIP/weather, source
metadata, Display/System без direct C radio calls.

**Критерий завершения:** ни один сервис не читает C radio в обход Domain.

### Этап 6 — Web и Display

**Исходные слои V2:** `web`, `display`.

**Назначение:** transport/client над canonical Domain.

**Зависимости:** Domain, Device service, `ha-model`.

**Что сделать:**

- Web: V2 snapshot/delta, per-client bounded queue, canonical records verbatim;
- Display: task/inbox, initial Domain read, widgets по capabilities;
- команды только через Domain/Device service;
- не вводить snapshot/subscription механизм, которого нет в V2.

**Проверки:** snapshot/delta, slow client, dropped inbox, Display rebuild,
navigation/commands, Wi-Fi через Domain.

**Критерий завершения:** Web/Display не являются source of truth и не обходят
Domain.

### Этап 7 — Composition root и runtime ownership

**Исходный слой V2:** `main`.

**Назначение:** только сборка, регистрация, запуск задач.

**Зависимости:** все предыдущие этапы.

**Что сделать:**

- убрать `intake` и polling queues из main;
- привести порядок запуска к V2 `app_main.c`;
- убрать бизнес-логику, публикацию системных событий и разбор records из main;
- оставить C bridges и platform startup.

**Проверки:** startup order, command executors registered before posts,
dispatcher before subscribers, no main drains.

**Критерий завершения:** main не содержит логики сервисов и не является
диспетчером.

### Этап 8 — Проверка V2-сценариев

**Назначение:** доказать эквивалентность на host и P4.

**Проверки:** первый старт/restore; Zigbee report→state; command path; interview/
configure/remove/leave; automation; system; Wi-Fi; Web; Display; persistence
policy; reboot after mutations; error/corrupt behavior; canonical parity;
32-device/8-property load.

**Критерий завершения:** все проверенные V2-сценарии проходят без изменения
поведения и без silent loss.

## 5. Правила портирования

- V2 — источник проверенного поведения; текущий Rust-код не является
  доказательством правильной архитектуры.
- Не менять алгоритмы и ABI без необходимости.
- Не добавлять функциональность, отсутствующую в V2.
- Использовать существующие Rust-компоненты.
- Не создавать параллельные реализации.
- Каждый слой подключать к Domain в соответствии с его ответственностью.
- Проверять эквивалентность поведения host-тестами и P4-сценариями.
- Временное отклонение фиксировать явно и удалять после замены.
- Не проектировать migration/recovery механизмы для данных в рамках порта.

### 5.1. Допустимые отличия от V2

V2 — основной источник архитектуры, алгоритмов и поведения обычных слоёв.
Портирование не требует буквального копирования каждой реализации.

Источники архитектуры:

| Область | Источник |
|---|---|
| Основная архитектура | проверенная V2 |
| Алгоритмы и бизнес-логика | проверенная V2 |
| Поведение обычных слоёв | проверенная V2 |
| Zigbee | текущая реализация на основе V2 + Z2M |
| Полезные Rust-улучшения | существующая реализация после оценки |
| Центральное состояние | существующий `ha-domain` |
| Низкоуровневое хранение | существующий `micro-db` |

Приоритет правил:

1. Обычные слои — архитектура, алгоритмы и поведение V2.
2. Zigbee — текущая целевая архитектура Zigbee с полезными механизмами Z2M.
3. Уже реализованные улучшения — индивидуальная оценка полезности и
   совместимости.

### 5.2. Особенности Zigbee-слоя

Zigbee — единственное заранее предусмотренное существенное архитектурное
отличие от V2.

- Текущий Zigbee-слой использует и V2-логику, и механизмы Zigbee2MQTT (Z2M).
- Его архитектура и поведение могут отличаться от Zigbee-слоя V2.
- Не нужно искусственно приводить его к V2.
- Нельзя трактовать отличия Zigbee как незавершённый порт V2.
- Zigbee обязан соблюдать общие контракты: Domain, Device service,
  canonical ABI, adapter-private данные и fire-and-forget command semantics.
- Полезные Z2M-механизмы (`z2m-rs` profiles/converters/configure,
  `zigbee-exec` request correlation/retry/expiry) сохраняются.
- Functional gaps закрываются относительно текущей Zigbee-цели, а не ради
  буквального совпадения с V2.

### 5.3. Уже реализованные улучшения

Уже существующее решение сохраняется только после проверки. Для каждого
сохранённого улучшения фиксируются: что реализовано, чем отличается от V2,
какую проблему решает и почему его нужно сохранить.

| Решение | Отличие от V2 | Проблема | Решение |
|---|---|---|---|
| `micro-db` Storage seam + `SimFlash` | Rust-разделение Engine/Storage и host-симулятор | проверка Flash-поведения без P4 | сохранить; поведение Table/Ring не менять |
| `DomainHandle`/`DomainTransaction` | Rust-обёртки владения и scoped lock | безопасные call sites и сериализация | сохранить, если V2-семантика сохраняется |
| `DomainMaintenance`/`SharedStorage` | явный maintenance seam | compaction без удержания Domain mutex | сохранить API; не подключать в Stage 1 и не расширять |
| `zigbee-exec` | TSN/correlation/retry/expiry и fake transport | надёжность ZCL-запросов | сохранить под Zigbee-исключением |
| `z2m-rs` generated profiles/converters | Z2M-модель, не V2 catalog | покрытие устройств и configure | сохранить под Zigbee-исключением |
| `ha-web` framing + host tests | Rust wire implementation | parity canonical protocol | сохранить; сервис привести к V2-поведению |
| ABI fixtures/const layout tests | Rust const/`#[test]` | защита canonical ABI | сохранить |

Нельзя считать решение полезным только потому, что оно уже реализовано.
Если пользы нет или оно создаёт лишнюю сложность, его нужно удалить или
заменить.

### 5.4. Решения, которые сохраняем

Сохраняем без возврата к буквальной V2-копии:

- `micro-db` как low-level storage primitives;
- `ha-model` canonical ABI;
- `ha-domain` как владелец canonical state;
- `DomainHandle`/`DomainTransaction` в пределах V2-контракта;
- Zigbee-архитектуру V2+Z2M и её публичные границы;
- `z2m-rs`, `zigbee-exec`, `zigbee-frames` как части Zigbee-слоя;
- host-test harness и ABI/parity fixtures;
- полезные Rust-адаптации, не меняющие внешнее поведение.

### 5.5. Временные и избыточные решения

| Решение | Статус | Действие |
|---|---|---|
| `store.rs` flat `save_domain`/`restore_domain`/`DOMAIN_DB` | временный параллельный storage path | заменить Domain-owned persistence на этапе 1 |
| `main` polling `intake`/`system::take`/`display::take_nav`/`wifi::take_scan`/`capabilities::take` | временная оркестрация | убрать на этапе 7 |
| Ручные offsets в automation/web/display/main | дублирование canonical ABI | заменить структурами `ha-model` |
| Прямой C Wi-Fi status в Display | обход Domain | заменить чтением Domain на этапе 5 |
| `Domain::with_mut` | переходный API | удалить после перевода call sites |
| Persistent `Entity`/`Capabilities`/`Logical Device` в текущем manifest | расхождение с V2 backing | привести к V2 на этапе 1 |
| Migration markers/recovery state machine | не относятся к порту | удалены из документации; не реализовывать |

Главный критерий: обычные слои портируем по V2; Zigbee развиваем как
V2+Z2M; полезные существующие Rust-улучшения сохраняем после технической
оценки. Это не разрешение на произвольное расширение архитектуры.

## 6. Текущая задача

**Stage 1 реализован; следующая задача — Stage 2 (Device service).**

Технический аудит: [DOMAIN_PERSISTENCE_AUDIT.md](../audits/DOMAIN_PERSISTENCE_AUDIT.md).
Диагностика раздела:
[DOMAIN_PARTITION_DIAGNOSTIC.md](../audits/DOMAIN_PARTITION_DIAGNOSTIC.md).
Явный сброс после решения оператора:
[DOMAIN_RESET_PROCEDURE.md](../audits/DOMAIN_RESET_PROCEDURE.md).

Решение: содержимое `domain` можно потерять; migration не выполняется.
Сброс остаётся отдельной явной операцией, не автоматическим поведением.

Перед первым запуском Stage 1 на устройстве всё ещё нужны:

1. read-only dump `domain` и SHA256;
2. фиксация решения оператора;
3. явный erase только `domain` в одном окне с прошивкой Stage 1.

Domain API parity закрыт: [DOMAIN_API_AUDIT.md](../audits/DOMAIN_API_AUDIT.md)
фиксирует исправленные ERROR-факты, выровненный payload contract, удалённый
production `with_mut` и документированные адаптации. Stage 2 остаётся после
финальной аппаратной проверки Stage 1 (уникальная persistence-запись).

## 7. Критерии завершения всего портирования

- Все необходимые слои V2 реализованы на Rust.
- Обычные слои подключены к Domain через свои V2-контракты.
- Zigbee реализует согласованную цель V2+Z2M и соблюдает общие контракты.
- Нет параллельных storage/lifecycle/command реализаций.
- Host-тесты и P4-сценарии V2 проходят.
- ABI, capacities, backing и startup order соответствуют V2 либо имеют явно
  обоснованное исключение (Zigbee или платформенная адаптация).
- Нет незавершённых migration-механизмов и новых архитектурных подсистем.
