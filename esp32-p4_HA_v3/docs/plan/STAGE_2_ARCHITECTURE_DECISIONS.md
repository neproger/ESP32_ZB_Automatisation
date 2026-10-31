# Stage 2 — архитектурное решение (предложение)

> Статус: **предложение для согласования**. Stage 2 не начат, код не менялся.
>
> Основание: `V3_IMPLEMENTATION_PLAN.md` §5, `STAGE_1D_ARCHITECTURE_DECISIONS.md`,
> `V3_LAYERS.md` и фактический код `zigbee-devices`, `ha-device`, `ha-app`,
> `ha-domain`.

## 1. Цель Stage 2

Разделить три ответственности, которые сейчас смешаны в `DomainAdapter` и
`ha-device::Sink`:

1. Zigbee adapter — только физический протокол и Zigbee-проекция.
2. Device service — canonical lifecycle Logical Device / Entity / ownership.
3. Domain — единственный владелец canonical state и persistence policy.

Stage 2 должен убрать из Zigbee adapter прямое владение canonical lifecycle,
generic CRUD и persistence. После Stage 2 adapter не должен создавать
Logical Device/Entity, не должен регистрировать Domain-таблицы и не должен
вызывать `RecordSink`/`StoreSink`.

## 2. Вне scope Stage 2

- Stage 3: System / Automation / Settings / Display.
- Stage 5: окончательная проводка Domain/storage в composition root.
- Stage 6: P4 и V2-scenarios.
- Изменение canonical ABI, раскладки разделов, Zigbee protocol или UI/Web.
- Удаление `with_mut` из `main`/zigbee ingest без отдельного transaction API.

## 3. Фактическое состояние

### 3.1. `DomainAdapter`

`crates/zigbee-devices/src/domain_adapter.rs` сейчас одновременно:

- регистрирует все canonical-таблицы (`register_tables`);
- создаёт Logical Device/Entity/Capabilities (`ensure_device`,
  `ensure_entity`, `ensure_standard_entity`);
- пишет state/capabilities;
- делает rename/remove lifecycle;
- имеет generic `put_record`/`remove_record` для групп, автоматизаций и
  настроек;
- удаляет группы/автоматизации/device records;
- передаёт все изменения в `RecordSink` для persistence.

Это не Zigbee projection, а второй canonical lifecycle/persistence слой.

### 3.2. `ha-device`

`ha-device` уже владеет neutral lifecycle (`register`, `bind_entity`, `rename`,
`remove`, remover registry), но:

- принимает `&mut dyn Sink`;
- пишет persistence через `Sink` внутри `ha-device`;
- `DomainAdapter` дублирует часть этого lifecycle.

### 3.3. Composition root

`main.rs`:

- вызывает `DomainAdapter::new` для регистрации таблиц;
- использует `DomainAdapter::with_sink` для групп/автоматизаций/настроек;
- реализует `StoreSink` через `store::save_domain`/`remove_domain`;
- до сих пор восстанавливает Domain через app-level `store::restore_domain`.

`Domain` уже имеет `StorageProvider`/`restore` (Stage 1c), но production
wiring запланирован на Stage 5.

### 3.4. `with_mut`

`DomainHandle::with_mut` используется для adapter/device mutation и
config-путей. В Stage 2 нельзя просто удалить его: lifecycle-операции
(register/bind/remove) меняют несколько canonical-записей и требуют
сериализации. Удаление `with_mut` требует отдельного Domain transaction API.

## 4. Целевое владение

```text
zigbee-devices
  EUI64, endpoint, ZCL, interview, bindings, Zigbee profile
  → canonical state projection
  → DomainHandle.entity_put(ENTITY_PROPERTY_STATE, ...)
  → HaDevice lifecycle calls

ha-device
  Logical Device / Entity / ownership / remover registry
  → canonical lifecycle через Domain

Domain
  canonical state + Journal + persistence policy

ha-app
  composition root: descriptors, storage, service startup
```

### 4.1. Zigbee adapter

Ответственность:

- Zigbee-specific identity (`EUI64`, endpoint);
- ZCL/frame/profile projection;
- interview/bindings/configure/leave;
- запись `PROPERTY_STATE` по report;
- вызов Device service для lifecycle;
- регистрация Zigbee remover.

Запрещено:

- регистрировать Domain tables;
- создавать/удалять Logical Device/Entity;
- generic CRUD групп/автоматизаций/настроек;
- иметь `RecordSink`/`StoreSink`;
- вызывать `store::save_domain` или storage backend.

### 4.2. Device service

Ответственность:

- `register` Logical Device;
- `ensure_entity`/`bind_entity`;
- canonical ownership;
- rename;
- remove + remover callback;
- capabilities lifecycle как neutral canonical data.

Device service не знает EUI64/ZCL; adapter передаёт ему canonical entity id,
device id и capabilities.

### 4.3. Domain

Domain остаётся владельцем persistence policy. Stage 2 не добавляет новых
storage-правил: используются существующие Stage 1c APIs.

## 5. Публичные контракты (candidate)

Точные сигнатуры утверждаются вместе с transaction-решением.

### 5.1. Device service (transitional)

`&mut Domain` ниже означает **только** вызов внутри утверждённого scoped
`with_mut`; это переходный контракт, а не разрешение другим сервисам напрямую
работать с Domain.

```rust
pub fn register(
    domain: &mut Domain,
    origin: u8,
    desc: &DeviceDesc<'_>,
) -> Result<DeviceId>;

pub fn ensure_entity(
    domain: &mut Domain,
    origin: u8,
    device: DeviceId,
    entity: EntityId,
    name: &str,
) -> Result<bool>;

pub fn set_capabilities(
    domain: &mut Domain,
    origin: u8,
    entity: EntityId,
    capabilities: &CapabilitiesRecord,
) -> Result<bool>;

pub fn bind_entity(...) -> Result<bool>;
pub fn rename(...) -> Result<bool>;
pub fn register_remover(...) -> Result<()>;
```

Многошаговое удаление разделяется:

```rust
pub struct RemovalPlan { /* source, native id, canonical keys */ }

pub fn prepare_remove(
    domain: &Domain,
    device: DeviceId,
) -> Result<RemovalPlan>;

pub fn invoke_remover(plan: &RemovalPlan) -> Result<()>; // вне Domain mutex

pub fn remove_canonical(
    domain: &mut Domain,
    origin: u8,
    plan: &RemovalPlan,
) -> Result<usize>;
```

Порядок: подготовить plan под коротким Domain read, отпустить Domain,
вызвать Zigbee remover вне Domain lock, затем под scoped `with_mut` удалить
canonical records. Ошибка remover не меняет canonical state. Если canonical
удаление частично успешно, операция возвращает ошибку и требует повтора;
полного rollback нет.

No `Sink`, `NoSink`, persistence callbacks. Persistence выполняется Domain,
когда production wiring появится в Stage 5; на host-этапе canonical state
остаётся в Domain.

| Операция | Записи | Гарантия при ошибке |
|---|---|---|
| `register` | Logical Device | одна запись; идемпотентно |
| `ensure_entity` | Entity + Capabilities | Entity может остаться без Capabilities; повторный вызов завершает |
| `set_capabilities` | Capabilities | одна запись |
| `bind_entity` | Entity ownership | одна запись; ownership проверяется до записи |
| `rename` | одна canonical запись | одна запись |
| `prepare_remove` | только чтение | без изменений |
| `invoke_remover` | внешний Zigbee callback | вне Domain lock; ошибка оставляет canonical state |
| `remove_canonical` | несколько записей | не атомарно; частичное удаление возможно, повтор идемпотентен |

`with_mut` даёт **сериализацию**, не атомарность. Частичная ошибка не
откатывается автоматически и должна быть видна вызывающему коду.

### 5.2. Zigbee projection

`PROPERTY_STATE` write/read идут через явные `DomainHandle` методы, без
публичного `&mut Domain`:

```rust
pub fn write_state(
    domain: &DomainHandle,
    entity: EntityId,
    property: PropertyId,
    value: Value,
) -> Result<bool>;

pub fn read_state(
    domain: &DomainHandle,
    entity: EntityId,
    property: PropertyId,
) -> Option<Value>;
```

`ensure_entity`, `ensure_device`, `rename`, `remove`, `put_record`,
`remove_group`, `remove_automation`, `remove_device_records` из
`DomainAdapter` удаляются или переносятся в Device service / временный
config-слой.

### 5.3. Descriptor bootstrap

`DomainAdapter::register_tables` не принадлежит Zigbee adapter. Definitions
descriptors остаются в архитектурных модулях, которые ими владеют
(`ha-model`/`ha-domain`/Zigbee projection). Composition root только собирает
эти definitions и регистрирует их; он не становится владельцем схемы.
Временная регистрация может жить в bootstrap helper (`ha-app`), окончательная
manifest policy — Stage 5.

## 6. Persistence boundary

### 6.1. Рекомендация и явные ограничения

- Удалить `RecordSink` и `ha_device::Sink` из adapter/device.
- Device service больше не вызывает persistence callbacks.
- На Stage 2 **не** подключать production Domain storage: это Stage 5.
- `StoreSink` может остаться только как временный config-слой в `ha-app`
  для ещё не мигрировавших групп/автоматизаций/настроек; после Stage 3 он
  удаляется.
- Ответственность за persistence остаётся у Domain; Stage 2 лишь убирает
  callback-путь adapter/device. Это не новая постоянная модель.

Последствия временного persistence gap:

| Аспект | До Stage 2 | После Stage 2 до Stage 5 |
|---|---|---|
| Device/Entity/Capabilities в RAM | да | да |
| Durable production persistence | через `StoreSink` | **отсутствует** |
| Изменения после reboot | сохраняются | **могут быть потеряны** |
| Crash consistency | по текущему store-пути | **не гарантируется** |
| `PROPERTY_STATE` | RAM-only | RAM-only, без изменений |
| Config groups/automation/settings | через `StoreSink` | временный config-слой |

Затронутые операции: `ha_device::register`, `ensure_entity`,
`set_capabilities`, `bind_entity`, `rename`, `remove_canonical` и создание
Entity/Capabilities Zigbee projection. Не затронут `PROPERTY_STATE`: он и
раньше был RAM-only.

Stage 2 **не считается готовым к production-развёртыванию с гарантией
сохранности Device/Entity**. Stage 5 обязан:

- подключить production Domain storage;
- восстановить durable save/restore для затронутых записей;
- проверить reboot/crash сценарии;
- только после этого вернуть production-гарантию persistence.

Host-тесты Stage 2 проверяют in-process Domain semantics, а не сохранность
после reboot. P4 на этом этапе не требуется.

### 6.2. Почему не тянуть wiring Domain storage в Stage 2

Wiring требует descriptor manifest, RegionManager, partition sizing и
production restore/erase policy — это Stage 5. Попытка сделать это в Stage 2
смешает adapter cleanup с composition-root/bootstrap и создаёт неутверждённую
миграцию.

## 7. Locking / transaction

### 7.1. Scoped `with_mut`

`with_mut` остаётся переходным механизмом **сериализации**, но не
атомарности. Lifecycle-операция, изменившая часть записей, может вернуть
ошибку с частично изменённым Domain; автоматического rollback нет.

Обязательные ограничения callback:

1. Только утверждённые lifecycle call sites.
2. Никакого executor, UI, Web или persistence callback внутри.
3. Никакого обращения к Zigbee stack или другим внешним сервисам.
4. Минимальная продолжительность удержания Domain mutex.
5. Static checker проверяет конкретные функции и места вызова.
6. Частичная ошибка документируется и возвращается вызывающему коду.

### 7.2. Remover вне Domain lock

Zigbee remover — внешний callback и не может вызываться под Domain mutex.
Удаление разделяется:

1. `prepare_remove` — короткое чтение под Domain lock, без мутации.
2. `invoke_remover` — вне Domain lock; ошибка не меняет canonical state.
3. `remove_canonical` — scoped `with_mut`, удаление canonical records;
   возможна частичная ошибка, повтор идемпотентен.

Порядок фиксирован: external remover не входит в `with_mut` closure.

### 7.3. Границы

- `DomainHandle` explicit methods для `PROPERTY_STATE` — только короткие
  операции, без `&mut Domain`.
- `&mut Domain` в Device service — только transitional аргумент внутри
  approved `with_mut`.
- Удаление `with_mut` из adapter/device переносится в Stage 3 или в отдельную
  задачу после появления Domain transaction API.
- Static checker разрешает `with_mut` только для утверждённых
  lifecycle/config call sites и запрещает внешние вызовы внутри callback.

## 8. Разрешённые изменения

- `crates/zigbee-devices`: projection API, удаление generic CRUD/sink,
  обновление тестов.
- `crates/ha-device`: lifecycle ownership, удаление Sink.
- `crates/ha-app`: механическая адаптация call sites, временный config-слой,
  bootstrap descriptors.
- `ha-domain`: только если lifecycle-операциям действительно нужен
  transaction/batch API; это отдельное решение.

Запрещено:

- менять canonical ABI или Zigbee protocol;
- менять System/Automation/Settings/Display логику;
- переносить business logic в `main.rs`;
- подключать production Domain storage вместо Stage 5.

## 9. Порядок работ

1. **2.1 Device service contracts**
   - убрать `Sink`/`NoSink` из `ha-device`;
   - добавить `ensure_entity`/`set_capabilities` без persistence;
   - зафиксировать таблицу multi-record операций и поведение при частичной
     ошибке;
   - разделить `prepare_remove` / `invoke_remover` / `remove_canonical`;
   - тесты lifecycle и частичных ошибок.
2. **2.2 Zigbee projection**
   - выделить `write_state`/`read_state` на `DomainHandle`;
   - удалить `RecordSink`, table registration, generic CRUD;
   - удалить создание Logical Device/Entity из adapter;
   - обновить tests.
3. **2.3 Call-site migration**
   - `zigbee_service`: Device service для register/ensure/bind/remove;
   - remover вызывается вне Domain mutex;
   - `main`: временный config-слой для ещё не мигрировавших CRUD;
   - удалить adapter sink.
4. **2.4 Delete legacy traits**
   - удалить `RecordSink` и `ha_device::Sink`;
   - удалить недостижимые adapter APIs;
   - обновить static checks: no sink, no store calls, no adapter table
     registration, no adapter lifecycle, no remover under lock, no
     unauthorized `with_mut`.
5. **2.5 Verification**
   - host tests + static checks for all bypass paths;
   - build `ha-app` допускается, P4 не требуется.

## 10. Критерии приёмки

- `zigbee-devices` не содержит `RecordSink`, `StoreSink`, generic
  `put_record`/`remove_group`/`remove_automation`, table registration.
- `ha-device` не содержит `Sink`/`NoSink` и persistence callbacks.
- Zigbee adapter не создаёт/не удаляет Logical Device/Entity.
- Device service владеет register/ensure/bind/rename/remove и capabilities.
- `PROPERTY_STATE` пишет только projection path через `DomainHandle`.
- Никакой adapter/device path не вызывает `store::save_domain`/`remove_domain`.
- Zigbee remover не вызывается внутри `with_mut`/Domain mutex.
- `main.rs` содержит только механическую адаптацию и временный config-слой;
  новой business logic нет.
- `cargo test --workspace` зелёный.
- `check-domain-api.ps1` обновлён под новый allowlist `with_mut` и проверяет
  отсутствие запрещённых вызовов внутри callback.
- Static checks дополнительно проверяют:
  - отсутствие `store::save_domain/remove_domain` в `zigbee-devices` и
    `ha-device`;
  - отсутствие регистрации canonical tables в Zigbee adapter;
  - отсутствие создания Logical Device/Entity в Zigbee adapter;
  - отсутствие generic CRUD в Zigbee adapter;
  - отсутствие неутверждённых `with_mut` call sites;
  - отсутствие remover callback под Domain lock.
- Host tests покрывают:
  - idempotent register/bind;
  - ownership/cascade remove;
  - capabilities lifecycle;
  - state projection;
  - частичную ошибку `ensure_entity` и повтор;
  - частичную ошибку `remove_canonical` и повтор;
  - внешний remover вне Domain lock;
  - отсутствие persistence callback;
  - отсутствие generic CRUD в adapter.
- P4 не требуется; production persistence gap до Stage 5 зафиксирован и
  включает явную таблицу затронутых операций/гарантий.

## 11. Риски и открытые решения

1. **Persistence gap:** подтверждён только как явно ограниченный временный
   режим. Production persistence Device/Entity отсутствует до Stage 5;
   после reboot изменения могут быть потеряны. Это не production-ready
   состояние и не новая постоянная модель.
2. **with_mut:** даёт сериализацию, но не атомарность. Частичные изменения
   возможны и не откатываются; нужны тесты на частичные ошибки.
3. **Remover:** внешний Zigbee callback обязан выполняться вне Domain mutex;
   иначе снова появляется внешняя работа под lock.
4. **Config CRUD:** временный `StoreSink` в `ha-app` не владеет схемой и не
   содержит бизнес-правил; его удаление — обязательный критерий Stage 3.
5. **Descriptor bootstrap:** composition root регистрирует definitions, но
   не владеет ими; окончательная manifest policy — Stage 5.
6. **Capabilities ownership:** Device service принимает canonical
   `CapabilitiesRecord`, но не знает Z2M profile; adapter остаётся
   источником projection.
7. **P4:** не входит в Stage 2; текущая отложенная валидация не меняет
   критерии.

## 12. Что требуется утвердить

1. Ownership: adapter только projection, Device service — lifecycle.
2. Удаление `RecordSink`/`ha_device::Sink` из adapter/device.
3. Persistence gap как ограниченный временный режим с явными гарантиями,
   а не как допустимая потеря данных в production.
4. Временный config-слой в `ha-app` до Stage 3; удаление обязательно.
5. Scoped `with_mut` как сериализация без атомарности; удаление после
   появления transaction API.
6. Разделённый remove: `prepare_remove` → remover вне lock →
   `remove_canonical`.
7. Точные Device service signatures и ownership descriptor definitions.

После утверждения можно начинать Stage 2; до этого production-код не
меняется.
