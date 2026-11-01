# Stage 2 — архитектурное решение (предложение)

> Статус: **историческая запись реализованных решений Stage 2**.
> Не активный план. Активный план:
> [V2_TO_RUST_PORTING_PLAN.md](V2_TO_RUST_PORTING_PLAN.md).
>
> Основание: `STAGE_1D_ARCHITECTURE_DECISIONS.md`, `V3_LAYERS.md` и фактический
> код `zigbee-devices`, `ha-device`, `ha-app`, `ha-domain`.

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
pub struct RemovalPlan {
    device: DeviceId,
    source: SourceId,
    native_id: Vec<u8>,          // bounded adapter identity
    device_version: u32,         // canonical Logical Device version
    entities: Vec<RemovalRecord>,
    related: RemovalFingerprint, // keys + versions for caps/state/group items
}

pub struct RemovalTicket {
    plan: RemovalPlan,
    native: NativeRemoval,
    progress: RemovalProgress,
}

pub struct RemovalProgress {
    // Preallocated bit per plan record; no allocation under Domain lock.
    completed: Vec<u8>,
}

pub enum NativeRemoval {
    Removed,
    AlreadyAbsent,
}

pub enum RemovalError {
    Remover(ha_domain::Error),
    StalePlan,
    Canonical(ha_domain::Error),
}

pub fn prepare_remove(
    domain: &Domain,
    device: DeviceId,
) -> Result<RemovalPlan, RemovalError>;

// Consumes the plan: the caller cannot invoke remover twice for one attempt.
pub fn invoke_remover(
    plan: RemovalPlan,
) -> Result<RemovalTicket, RemovalError>; // вне Domain mutex

// Mutable ticket: progress is recorded in the same Domain-lock operation
// as each successful canonical deletion.
pub fn remove_canonical(
    domain: &mut Domain,
    origin: u8,
    ticket: &mut RemovalTicket,
) -> Result<usize, RemovalError>;
```

Порядок: `prepare_remove` под коротким Domain read, `invoke_remover` вне
Domain lock, затем `remove_canonical` под scoped `with_mut`. Ticket
удерживается caller'ом до успешного canonical cleanup. `RemovalProgress`
аллоцируется в `prepare_remove`/`invoke_remover` и только маркируется под
Domain lock; новые аллокации в cleanup не выполняются.

Контракт remover:

- Повторный вызов для уже отсутствующего устройства должен возвращать
  `NativeRemoval::AlreadyAbsent` и считаться успехом; adapter может
  реализовать это через свой `NotFound` → `AlreadyAbsent`.
- Ошибка remover отделена от `Canonical` и не меняет canonical state.
- После успешного `invoke_remover` повторный canonical cleanup выполняется
  только по тому же `RemovalTicket`, без повторного вызова remover.

Защита от устаревшего плана и учёт прогресса:

- `RemovalPlan` фиксирует `DeviceId`, `source`, native id, версию Logical
  Device, ожидаемые Entity records и fingerprint связанных записей.
- `remove_canonical` перед первой мутацией повторно проверяет plan под
  `with_mut`: существующие planned records обязаны совпадать по identity и
  version; ownership обязан совпадать с device; новые owned entities,
  которых нет в плане, делают план stale.
- Каждая запись плана имеет свой progress bit. Успешное удаление и установка
  bit выполняются в одной критической секции Domain, без fallible-шагов
  между ними. `RemovalProgress` заранее аллоцирован, поэтому отметка не
  может отказать по памяти.
- Повторная попытка:
  - bit установлен и запись отсутствует — запись уже завершена, skip;
  - bit установлен и запись снова существует — план stale;
  - bit не установлен и запись отсутствует — план stale (нельзя доказать,
    что её удалил именно этот ticket);
  - bit не установлен и запись существует — повторная проверка
    identity/version/ownership, затем удаление и установка bit.
- Если plan stale, `remove_canonical` не удаляет ничего и возвращает
  `StalePlan`; вызывающий обязан сделать новый `prepare_remove`/ticket, а не
  удалять записи по старому плану.
- Частичный canonical cleanup не откатывается. Retry с тем же ticket
  продолжает с записей без progress bit; если ownership/version изменились,
  возвращается `StalePlan`.
- При потере ticket безопасное восстановление идёт через новый
  `prepare_remove` по текущему Domain state. `AlreadyAbsent` remover делает
  этот путь допустимым; старый progress не переносится и не угадывается.

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
| `invoke_remover` | внешний Zigbee callback | вне Domain lock; ошибка оставляет canonical state; repeated absent → успех |
| `remove_canonical` | несколько записей | revalidate; progress bit ставится после успешного удаления под тем же lock; stale → без мутаций; retry по ticket |

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

1. `prepare_remove` — короткое чтение под Domain lock, без мутации;
   `RemovalPlan` фиксирует identity/version snapshot.
2. `invoke_remover` — вне Domain lock, потребляет plan и возвращает
   `RemovalTicket`; `AlreadyAbsent` — успех; ошибка remover не меняет
   canonical state.
3. `remove_canonical` — scoped `with_mut`, принимает `&mut RemovalTicket`,
   сначала revalidate plan, затем удаляет canonical records и сразу
   маркирует progress bit в той же критической секции. Stale plan — без
   мутаций; частичная ошибка — retry по тому же ticket без повторного
   remover. Missing record без progress bit — stale, а не «уже завершено».

Порядок фиксирован: external remover не входит в `with_mut` closure;
canonical cleanup не вызывает remover повторно; progress обновляется только
под Domain lock.

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
   - реализовать `RemovalPlan`/`RemovalTicket`, preallocated
     `RemovalProgress`, stale-проверку ownership/version и idempotent
     `AlreadyAbsent`;
   - тесты lifecycle, частичных ошибок, retry, progress bitmap и stale plan.
2. **2.2 Zigbee projection**
   - выделить `write_state`/`read_state` на `DomainHandle`;
   - удалить `RecordSink`, table registration, generic CRUD;
   - удалить создание Logical Device/Entity из adapter;
   - обновить tests.
3. **2.3 Call-site migration**
   - `zigbee_service`: Device service для register/ensure/bind/remove;
   - remover вызывается вне Domain mutex и только один раз на ticket;
   - progress обновляется в той же критической секции, что и удаление;
   - повторный canonical cleanup после ошибки идёт по тому же ticket;
   - потеря ticket восстанавливается через новый `prepare_remove` и
     `AlreadyAbsent`, без угадывания старого progress;
   - stale `RemovalPlan` не удаляет записи и требует нового prepare;
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
  - частичную ошибку `remove_canonical` и retry по тому же ticket без
    повторного remover;
  - remover успешен, canonical cleanup частично неуспешен, повтор cleanup
    завершает удаление;
  - повторный remover для уже отсутствующего устройства →
    `AlreadyAbsent` без ошибки;
  - ownership/version изменились между prepare и cleanup →
    `StalePlan`, без мутаций;
  - устаревший plan не удаляет чужие или заново созданные записи;
  - progress bit устанавливается только после успешного удаления в той же
    критической секции;
  - отсутствующая запись без progress bit → `StalePlan`, без удалений;
  - потеря ticket → новый `prepare_remove` + `AlreadyAbsent` восстанавливает
    cleanup без угадывания старого progress;
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
   повторный вызов для уже удалённого устройства должен возвращать
   `AlreadyAbsent`, а не создавать неопределённую ошибку.
4. **Stale plan:** ownership/version могут измениться между prepare и
   canonical cleanup; такой plan обязан отказаться от удаления, а не
   затрагивать новое состояние.
5. **Progress/ticket:** отсутствие записи без progress bit нельзя считать
   завершённым; потеря ticket восстанавливается через новый prepare и
   `AlreadyAbsent`. Progress/битовая карта не являются Domain state.
6. **Config CRUD:** временный `StoreSink` в `ha-app` не владеет схемой и не
   содержит бизнес-правил; его удаление — обязательный критерий Stage 3.
7. **Descriptor bootstrap:** composition root регистрирует definitions, но
   не владеет ими; окончательная manifest policy — Stage 5.
8. **Capabilities ownership:** Device service принимает canonical
   `CapabilitiesRecord`, но не знает Z2M profile; adapter остаётся
   источником projection.
9. **P4:** не входит в Stage 2; текущая отложенная валидация не меняет
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
   `remove_canonical`; `RemovalTicket` предотвращает повторный remover.
7. Progress contract: preallocated bitmap, marking under same Domain lock,
   missing without bit → stale, ticket loss recovery via new prepare.
8. Идемпотентность: `AlreadyAbsent` — успех; stale plan → `StalePlan` без
   мутаций.
9. Точные Device service signatures и ownership descriptor definitions.

После утверждения можно начинать Stage 2; до этого production-код не
меняется.
