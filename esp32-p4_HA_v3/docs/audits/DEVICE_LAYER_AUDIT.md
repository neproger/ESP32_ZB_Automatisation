# Аудит Device Layer: V2 → Rust

> Статус: **аудит + Stage 2 host-верификация**. P0-решения приняты,
> код обновлён; P4 для Stage 2 не требуется.
> V2: `esp32-p4_HA_v2/device/`, `docs/STEP_B_PLAN.md`.
> Rust: `crates/ha-device`, `ha-app/zigbee_service.rs`,
> `ha-app/domain_tables.rs`, `ha-app/store.rs`.

## 1. Контракт V2

Публичный API `device/include/device/device_service.h`:

- `device_register(domain, origin, desc, out_device_id)`;
- `device_bind_entity(domain, origin, device_id, entity_id)`;
- `device_remove(domain, origin, device_id)`;
- `device_register_remover(source, fn, ctx)`;
- `device_rename(domain, origin, device_id, name)`.

Отдельного start/stop/task lifecycle у V2 Device service нет: это
синхронные операции над Domain, вызываемые adapter/UI.

### 1.1. Identity

```text
device_id        = ha_device_id_derive(source, native_id)
parent_device_id = ha_device_id_derive(source, parent_native_id)
```

`native_id` — opaque строка, канонизация adapter-side. Device id не
персистится.

### 1.2. Модель

```text
Logical Device: { source, parent_device_id, native_id, name }
Entity:         { entity_id, device_id, name }
```

`Logical Device` и `Entity` — RAM-таблицы V2; физическая привязка
`Entity ↔ EUI64+endpoint` остаётся adapter-private.

### 1.3. Поведение операций

- `register`: upsert Logical Device; существующее имя не перетирается;
  initial name заполняется только если пусто; идемпотентно.
- `bind_entity`: Entity должна существовать; повторная привязка к тому же
  Device — OK; к другому — `INVALID_STATE`; иначе `NOT_FOUND`.
- `remove`: прочитать Device; найти remover по `record.source`; вызвать
  remover; при ошибке canonical cleanup не выполнять; затем удалить
  property state, group item, capabilities, Entity, Logical Device.
- `rename`: меняет только LogicalDevice.name; Entity.name не трогает.
- `register_remover`: до 4 source; повторная регистрация source
  идемпотентна.
- Automation-ссылки при removal сохраняются (broken ref допустим).
- Parent-child cascade не реализован в V2.

### 1.4. Порядок removal V2

```text
UI -> Device service
  -> read Logical Device
  -> remover(source, native_id)
  -> cascade Entity/state/capabilities/group_item
  -> remove Logical Device
```

Ошибка remover останавливает canonical cleanup.

## 2. Текущий Rust

`ha-device`:

- `register` — Logical Device upsert; identity/name semantics V2;
- `bind_entity` — ownership bind; V2 semantics;
- `rename` — Logical Device rename; V2 semantics;
- `register_remover` — до 4 source; V2 semantics;
- `ensure_entity` — **дополнительная** операция: создаёт Entity и
  выставляет owner;
- `set_capabilities` — **дополнительная** операция: пишет capabilities
  после проверки owner;
- `rename_entity` — **удалена** из Device service; Entity rename остаётся
  app-level V2 command path (`HA_CMD_ENTITY_RENAME`);
- `prepare_remove` — snapshot/plan removal;
- `invoke_remover` — adapter remover по source;
- `remove_canonical` — version-checked cascade по плану.

`CanonicalStore` trait позволяет `ha-device` работать с `Domain` и
`DomainTransaction`; это Rust-адаптация, не V2 endpoint.

### 2.1. Текущий ownership

```text
Zigbee adapter
  -> DeviceDesc(source, native_id, parent, name)
  -> ha_device::register            (Logical Device)
  -> ha_device::ensure_entity       (Entity + owner)
  -> ha_device::set_capabilities    (Capabilities)
  -> ha_device::bind_entity         (ownership; idempotent)
Domain
  -> Logical Device / Entity / Capabilities (RAM)
Zigbee private
  -> Registry, bindings Entity ↔ EUI64+endpoint
```

Removal:

```text
UI/intake -> zigbee_service::remove_device
  -> ha_device::prepare_remove(domain, device)
  -> ha_device::invoke_remover(plan)       (Zigbee remover)
  -> ha_device::remove_canonical(domain, SOURCE_UI, ticket)
  -> zigbee_service clears adapter bindings
```

Domain не вызывает adapter; adapter remover регистрируется через
`ha_device::register_remover`.

## 3. Матрица V2 → Rust

| V2 | Rust | Статус |
|---|---|---|
| `device_register` | `register` | **Equivalent** (Rust trait/transaction adaptation) |
| `device_bind_entity` | `bind_entity` | **Equivalent** |
| `device_remove` | `prepare_remove` + `invoke_remover` + `remove_canonical` | **Adapted** (split plan/ticket, version protection) |
| `device_register_remover` | `register_remover` | **Equivalent** |
| `device_rename` | `rename` | **Equivalent** |
| start/stop lifecycle | — | **Equivalent** (V2 has none) |
| Entity creation | `ensure_entity` | **Adapted** (accepted Rust/Z2M, not V2 parity) |
| Capabilities write | `set_capabilities` | **Adapted** (accepted Rust/Z2M, not V2 parity) |
| Entity rename | app-level command path | **Equivalent to V2 executor**, not Device service |
| Identity derivation | canonical `device_id_derive` | **Equivalent** |
| RAM persistence policy | Domain manifest RAM | **Equivalent** |
| Adapter-private registry | Zigbee `Registry` in `mstore` | **Equivalent** |
| Removal order | plan + remover + cascade | **Adapted** |
| Automation refs retained | retained | **Equivalent** |
| Parent-child cascade | absent | **Equivalent** (V2 also absent) |

## 4. Расхождения и потенциальные ошибки

### P0 — boundary of Entity/Capabilities lifecycle — resolved

Accepted:

- `ensure_entity` and `set_capabilities` remain Device service operations;
  both are **Rust/Z2M adaptations**, not V2 parity;
- `rename_entity` is removed from Device service. V2 has a separate
  app-level `HA_CMD_ENTITY_RENAME` executor; the Rust app keeps that
  path without adding a Device service contract.

### P0 — adapter must not duplicate Device lifecycle — resolved

Zigbee adapter forms Zigbee identity, canonical Entity ID and Capabilities
data, then calls Device service. It publishes `PROPERTY_STATE` through
`DomainAdapter::write_state`/`read_state`. No direct canonical writes for
LogicalDevice/Entity/Capabilities are allowed in adapter code.

### P1 — split removal vs V2 one-call removal

V2 `device_remove` is one synchronous operation. Rust exposes three
operations (`prepare_remove`, `invoke_remover`, `remove_canonical`).
This adds retry/version protection, but changes the public lifecycle:
callers must coordinate all three. Audit decision: either keep split as
documented adaptation or add a V2-shaped `remove` wrapper; do not leave
callers to improvise.

### P1 — remover-before-cascade failure

Both V2 and Rust call adapter remover before canonical cascade. If
canonical removal fails after a successful remover, physical object may
already be removed while canonical records remain. V2 has the same
behavior; do not silently “fix” it. Document as known V2 semantics.

### P1 — `remove_canonical` uses `SOURCE_UI` — checked

V2 `WEB_CMD_LOGICAL_DEVICE_REMOVE` calls `device_remove(...,
DOMAIN_SOURCE_UI, ...)`; the Rust removal intake is also Web/UI-only.
`SOURCE_UI` therefore matches the current V2 call path. Do not substitute
another origin until a non-UI removal caller exists.

### P1 — stale-plan semantics

Rust `validate_plan` rejects a plan if a record's version changed or a
new owned Entity appeared. V2 re-scans current Entities and removes them.
Rust is safer for concurrent changes but differs from V2. This is an
adaptation; document and test.

### P2 — missing NotFound handling — checked, no change

V2 returns `NOT_FOUND` when the Logical Device is absent; Rust
`prepare_remove` returns `Canonical(NOT_FOUND)` before any mutation.
During cascade, a missing record without progress is deliberately
`StalePlan` (approved decision), not silently ignored. No code change.

### P2 — removal ticket progress

`RemovalTicket`/progress are in-memory only and are not persisted; no
background task or recovery state was added. The production Zigbee path
does not retry a dropped ticket. Keep as the local contract for
`remove_canonical`; do not add persistence without a consumer.

### P2 — parent-child cascade

V2 explicitly leaves parent-child cascade future. Rust also does not
cascade children. Do not add during port.

## 5. Ownership / lifecycle map

| Resource | Owner |
|---|---|
| Logical Device canonical record | Device service, stored in Domain |
| Entity canonical ownership | Device service, stored in Domain |
| Capabilities | Device service in current Rust; V2 adapter |
| Entity.name | App-level V2 `HA_CMD_ENTITY_RENAME`; not Device service |
| EUI64/endpoint/cluster/ZCL | Zigbee adapter-private |
| Physical removal | Adapter remover registered by source |
| Registry/persistence | Zigbee adapter-private `mstore` |
| Removal orchestration | UI -> adapter service -> Device service |
| Domain | storage/runtime; does not call adapter |

## 6. Предлагаемый порядок

### P0

1. `ensure_entity`/`set_capabilities` закреплены как Device service
   adaptations; `rename_entity` удалена из Device service.
2. Проверить отсутствие прямых canonical writes Lifecycle в Zigbee
   adapter.
3. Подтвердить RAM-persistence Logical Device/Entity/Capabilities и
   adapter-private registry.

### P1

4. Решить форму removal API: оставить split `prepare/invoke/remove` как
   документированную адаптацию или добавить V2-shaped wrapper.
5. Проверить origin removal, NotFound и stale-plan behavior.
6. Добавить host tests на V2 parity: register/idempotency, bind,
   remove cascade, remover failure, rename, parent identity.

### P2

7. Решить судьбу `RemovalTicket`/progress: оставить как retry mechanism
   или удалить.
8. Проверить parent-child behavior (не добавлять cascade).
9. Обновить `DEVICE` documentation and hardware scenarios.

## 7. Итог

Device service в основном перенесён: identity, register, bind, rename
Logical Device, remover и removal cascade совпадают по смыслу с V2.
`ensure_entity`/`set_capabilities` приняты как Rust/Z2M adaptations;
Entity rename вынесен из Device service в app-level V2 command path.
Removal остаётся разделённым на plan/ticket/cascade как адаптация границы
Domain lock.
