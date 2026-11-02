# Аудит Domain API: V2 → Rust

> Статус: **Domain API parity завершён; Stage 2 gate пройден по API.**
> Источник V2: `esp32-p4_HA_v2/domain/include/domain/domain.h`.
> Текущий API: `crates/ha-domain/src/`.
>
> Все 15 верхнеуровневых entry points V2 имеют Rust-эквивалент. Статусы ниже
> разделены на `Equivalent`, `Adapted` и `Incomplete`.

## 1. Текущая публичная поверхность

Production-путь:

```text
Domain::new_with_storage
Domain::register_entity
Domain::restore
DomainHandle::new
DomainHandle::transaction
DomainHandle::entity_put/get/meta/remove
DomainHandle::entity_snapshot
DomainHandle::payload_put/get
DomainHandle::register_command
DomainHandle::post
DomainHandle::dispatch_once
DomainHandle::dispatch_handle
```

Дополнительно:

- `DomainHandle::with_mut` — **только для тестов**, production call sites
  отсутствуют;
- `DomainMaintenance` / `take_maintenance`, `gaps`, `dropped_facts`,
  `subscriber_drops` — diagnostics/maintenance;
- `Domain::new`, `new_with_clock` — host-test seams;
- `EntitySnapshot`, `EntityMeta`, `Filter`, `Fact`, `FactKind`, `Op`;
- storage seams: `StorageProvider`, `StorageBackend`, `SharedStorage`,
  `Backing`, `RestoreReport`, `RestoreError`.

## 2. Соответствие entry points V2

| V2 API | Rust | Статус |
|---|---|---|
| `domain_init` | `Domain::new*` + `new_with_storage` + `restore` | **Adapted** |
| `domain_deinit` | Rust `Drop`/RAII | **Equivalent** |
| `domain_register_entity` | `Domain::register_entity` | **Adapted** (pure `Flash` не поддержан) |
| `domain_entity_put` | `Domain::entity_put` / handle | **Equivalent** |
| `domain_entity_get` | `Domain::entity_get` | **Equivalent** |
| `domain_entity_meta` | `Domain::entity_meta` | **Equivalent** |
| `domain_entity_remove` | `Domain::entity_remove` | **Equivalent** |
| `domain_entity_iter` | `Domain::snapshot` / handle snapshot | **Adapted** (bounded snapshot) |
| `domain_payload_put` | `Domain::payload_put` / handle | **Equivalent** |
| `domain_payload_get` | `Domain::payload_get` | **Equivalent** |
| `domain_register_command` | `Domain::register_command` / handle | **Equivalent** |
| `domain_post` | `DomainHandle::post` | **Equivalent** |
| `domain_subscribe` | `Domain::subscribe_channel` | **Adapted** (`SyncSender`) |
| `domain_dispatch_once` | `Domain::dispatch_once` / handle | **Equivalent** |
| `domain_dispatch_wait` | `dispatch_handle().wait()` | **Adapted** (no timeout) |

## 3. Закрытые расхождения

### P0 — ERROR-факты

Исправлено по V2:

- `entity_put` пишет ERROR при storage-сбое;
- `entity_get` пишет ERROR при storage-сбое, `NotFound` — без факта;
- `entity_meta` пишет ERROR при storage-сбое, `NotFound` — без факта;
- snapshot/iter пишет ERROR при storage-сбое, `NotFound` — без факта;
- `payload_put` пишет ERROR при отказе ring;
- `entity_remove(NotFound)` больше не пишет ERROR;
- прочие storage-ошибки `entity_remove` пишут ERROR.

Read-path ERROR-факты используют `SOURCE_SYSTEM`, как V2 при `meta == NULL`.

### P1 — payload size

`payload_get` теперь требует точного размера буфера, как V2. Слишком большой
или слишком маленький буфер — `INVALID_SIZE`.

### P2 — filter bits

`FactKind::bit()` теперь использует V2-масштаб `1 << kind`.

### P2 — duplicate command

Повторная регистрация команды возвращает `INVALID_STATE`, как V2.

## 4. Осознанные адаптации

| Область | Решение |
|---|---|
| `entity_iter` | Оставлен `EntitySnapshot` с лимитом 16 KiB. Callback iterator не добавлен: текущие таблицы помещаются в лимит, callback-потребителя нет. |
| `dispatch_wait` | `DispatchHandle::wait()` без timeout. Production использует бесконечное ожидание; timeout не нужен текущим задачам. |
| `subscribe` | `SyncSender` вместо callback `try_push/wake`; потери inbox считаются. |
| `domain_deinit` | Заменён RAII; отдельный endpoint не нужен. |
| `DomainMaintenance` | Существующий maintenance API; не подключается в startup. |
| `gaps`/`dropped_facts` | Диагностика; production не обязана их читать. |

`Backing::Flash` остаётся `Incomplete`: Rust отклоняет pure-Flash descriptor.
Сейчас ни одна production-таблица его не использует; при появлении потребителя
это отдельное решение, а не скрытая адаптация.

## 5. `with_mut` — миграция завершена

| Call site | Что делал | Замена | Результат |
|---|---|---|---|
| `main.rs` capabilities mailbox | `ha_device::set_capabilities` | `DomainHandle::transaction` + `DomainTransaction` | поведение сохранено |
| `main.rs` rename entity | Device service rename | transaction + `ha_device::rename_entity` | поведение сохранено |
| `main.rs` rename device | Device service rename | transaction + `ha_device::rename` | поведение сохранено |
| `zigbee_service.rs` register device | Device service register | transaction + `ha_device::register` | ответственность осталась в Device service |
| `zigbee_service.rs` ensure entity/capabilities | Device service ensure/capabilities | transaction + `ha_device` | Zigbee не получил Domain lifecycle |
| `zigbee_service.rs` bind entity | Device service bind | transaction + `ha_device::bind_entity` | поведение сохранено |
| `zigbee_service.rs` apply capabilities | Device service capabilities | transaction + `ha_device::set_capabilities` | поведение сохранено |

Для этого `ha-device` получил узкий `CanonicalStore` trait, реализованный для
`Domain` и `DomainTransaction`. Это не перенос lifecycle в Zigbee: Device
service по-прежнему владеет lifecycle, а транзакция только задаёт Domain
critical section.

`DomainHandle::with_mut` остаётся только тестовым seam. Production call sites:
**0**.

## 6. Проверки

- `cargo test --workspace` — passed;
- `tools/build-ha-app.ps1` — passed;
- `tools/check-domain-api.ps1` — ok;
- warnings в `micro-db`/`z2m-rs` — cleaned;
- `cargo fmt` для изменённых crate — clean;
- `git diff --check` — clean.

## 7. Итог

Domain API готов для Stage 2:

- P0 error-path исправлены;
- payload contract выровнен;
- filter/command error semantics выровнены;
- production `with_mut` отсутствует;
- оставшиеся отличия (`snapshot`, no-timeout wait, `SyncSender`, pure
  `Backing::Flash`) документированы как адаптации и не блокируют перенос
  следующих слоёв.
