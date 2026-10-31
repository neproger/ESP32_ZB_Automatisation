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

### 5.1. Device service

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
pub fn remove(...) -> Result<usize>;
pub fn register_remover(...) -> Result<()>;
```

No `Sink`, `NoSink`, persistence callbacks. Persistence выполняется Domain,
когда production wiring появится в Stage 5; на host-этапе canonical state
остаётся в Domain.

### 5.2. Zigbee projection

```rust
pub fn write_state(
    domain: &mut Domain,
    entity: EntityId,
    property: PropertyId,
    value: Value,
) -> Result<bool>;

pub fn read_state(
    domain: &Domain,
    entity: EntityId,
    property: PropertyId,
) -> Option<Value>;
```

`ensure_entity`, `ensure_device`, `rename`, `remove`, `put_record`,
`remove_group`, `remove_automation`, `remove_device_records` из
`DomainAdapter` удаляются или переносятся в Device service / временный
config-слой.

### 5.3. Descriptor bootstrap

`DomainAdapter::register_tables` не принадлежит Zigbee adapter. На время
Stage 2 descriptors переносятся в composition root (`ha-app::store` или
отдельный bootstrap helper); окончательная manifest-политика — Stage 5.
Domain не получает магических знаний о Zigbee.

## 6. Persistence boundary

### 6.1. Рекомендация

- Удалить `RecordSink` и `ha_device::Sink` из adapter/device.
- Device service больше не вызывает persistence callbacks.
- На Stage 2 **не** подключать production Domain storage: это Stage 5.
- `StoreSink` может остаться только как временный config-слой в `ha-app`
  для ещё не мигрировавших групп/автоматизаций/настроек; после Stage 3 он
  удаляется.
- Считать это осознанным переходным долгом: после Stage 2 Zigbee
  Device/Entity lifecycle не имеет production persistence до Stage 5.
  Host-тесты от этого не зависят; P4 на этом этапе не требуется.

### 6.2. Почему не тянуть wiring Domain storage в Stage 2

Wiring требует descriptor manifest, RegionManager, partition sizing и
production restore/erase policy — это Stage 5. Попытка сделать это в Stage 2
смешает adapter cleanup с composition-root/bootstrap и создаёт неутверждённую
миграцию.

## 7. Locking / transaction

- Stage 2 не добавляет новых публичных блокировок.
- Lifecycle операции, меняющие несколько записей, остаются в scoped
  `DomainHandle::with_mut` до появления явного Domain transaction API.
- `with_mut` не должен вызывать executor/UI/Web и не должен вызывать
  persistence sink.
- Удаление `with_mut` из adapter/device переносится в Stage 3 или в отдельную
  задачу после Stage 2; для этого нужно решить, нужен ли atomic lifecycle
  transaction API.
- Static checker должен разрешать `with_mut` только для утверждённых
  lifecycle/config call sites.

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
   - тесты lifecycle.
2. **2.2 Zigbee projection**
   - выделить `write_state`/`read_state`;
   - удалить `RecordSink`, table registration, generic CRUD;
   - обновить tests.
3. **2.3 Call-site migration**
   - `zigbee_service`: Device service для register/ensure/bind/remove;
   - `main`: временный config-слой для ещё не мигрировавших CRUD;
   - удалить adapter sink.
4. **2.4 Delete legacy traits**
   - удалить `RecordSink` и `ha_device::Sink`;
   - удалить недостижимые adapter APIs;
   - обновить static checks.
5. **2.5 Verification**
   - host tests + no-sink grep;
   - build `ha-app` допускается, P4 не требуется.

## 10. Критерии приёмки

- `zigbee-devices` не содержит `RecordSink`, `StoreSink`, generic
  `put_record`/`remove_group`/`remove_automation`, table registration.
- `ha-device` не содержит `Sink`/`NoSink` и persistence callbacks.
- Zigbee adapter не создаёт/не удаляет Logical Device/Entity.
- Device service владеет register/ensure/bind/rename/remove и capabilities.
- `PROPERTY_STATE` пишет только projection path через Domain.
- Никакой adapter/device path не вызывает `store::save_domain`/`remove_domain`.
- `main.rs` содержит только механическую адаптацию и временный config-слой;
  новой business logic нет.
- `cargo test --workspace` зелёный.
- `check-domain-api.ps1` обновлён под новый allowlist `with_mut`.
- Host tests покрывают:
  - idempotent register/bind;
  - ownership/cascade remove;
  - capabilities lifecycle;
  - state projection;
  - отсутствие persistence callback;
  - отсутствие generic CRUD в adapter.
- P4 не требуется; production persistence gap до Stage 5 зафиксирован.

## 11. Риски и открытые решения

1. **Persistence gap:** после удаления sink Device/Entity config не
   сохраняется до Stage 5. Нужно утвердить как допустимый переходный риск.
2. **with_mut:** без transaction API lifecycle остаётся под scoped lock.
   Удаление `with_mut` — Stage 3/отдельная задача; иначе нужен
   Domain transaction contract.
3. **Config CRUD:** временный `StoreSink` в `ha-app` сохраняет старый путь
   для групп/автоматизаций; его удаление — Stage 3.
4. **Descriptor bootstrap:** перенос регистрации таблиц в composition root
   должен быть временным; окончательная manifest policy — Stage 5.
5. **Capabilities ownership:** Device service принимает canonical
   `CapabilitiesRecord`, но не знает Z2M profile; adapter остаётся
   единственным источником projection.
6. **P4:** не входит в Stage 2; текущая отложенная валидация не меняет
   критерии.

## 12. Что требуется утвердить

1. Ownership: adapter только projection, Device service — lifecycle.
2. Удаление `RecordSink`/`ha_device::Sink` из adapter/device.
3. Persistence gap до Stage 5 как допустимый переходный риск.
4. Временный config-слой в `ha-app` до Stage 3.
5. Сохранение scoped `with_mut` для lifecycle до появления transaction API.
6. Точные Device service signatures и bootstrap-владелец descriptors.

После утверждения можно начинать Stage 2; до этого production-код не
меняется.
