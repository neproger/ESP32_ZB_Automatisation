# Stage 1d.3 — Implementation Plan

> Статус: **план реализации для согласования**. Код не менялся.
>
> Архитектурная основа: `STAGE_1D_ARCHITECTURE_DECISIONS.md` (`fd94da8`).
> Границы: только `DomainHandle`, command execution, call sites в `ha-app`,
> snapshot-чтение UI/Web/Automation и тесты.

## 1. Цели

1. Убрать публичный `DomainHandle::lock()` и `DomainHandle::with()`.
2. Не выполнять executor под Domain mutex.
3. Сохранить последовательное выполнение команд и точную семантику
   `COMMAND_SENT`.
4. Перевести UI/Web/Automation на owned snapshot/короткие read-операции.
5. Оставить `with_mut` только переходным adapter/device исключением.
6. Доказать тестами отсутствие deadlock и сохранение Domain semantics.

## 2. Не входит в Stage 1d.3

- `RecordSink`/`StoreSink` и adapter persistence cleanup.
- Production wiring, `main.rs` bootstrap новой per-entity схемы.
- `RegionManager`, micro-db, Zigbee protocol.
- Удаление `with_mut` (Stage 2).
- Новые entity types/capacities.
- P4/hardware validation.

## 3. Последовательность работ

### Phase 0 — Baseline и фиксация текущего поведения

1. Зафиксировать текущие тесты `ha-domain`, `micro-db`, workspace.
2. Добавить characterization-тесты текущей семантики:
   - `COMMAND_SENT` только при executor `Ok`;
   - `COMMAND_SENT` отсутствует при `Err`;
   - `put/remove` и storage errors не меняют факты Stage 1c;
   - `entity_remove(NotFound)` пока остаётся отдельным дефектом.

**Gate 0:** baseline зелёный; behavior зафиксирован тестами.

### Phase 1 — Command execution внутри `ha-domain`

1. Заменить command registry на разделяемый handler:
   `Arc<dyn Fn(&[u8]) -> Result<()> + Send + Sync>`.
2. Ввести private `PreparedCommand`:
   - owned args;
   - owned target key/meta;
   - handler `Arc`;
   - command type.
3. Разделить `Domain::post` на:
   - подготовку под Domain lock;
   - выполнение handler без Domain lock;
   - публикацию `COMMAND_SENT` под Domain lock.
4. Добавить command lane:
   - захватывается после подготовки;
   - удерживается до публикации `COMMAND_SENT`;
   - очередь не вводится, строгий FIFO не обещается.
5. Добавить reentrant detection до любых mutex:
   - thread-local признак executor;
   - thread-local признак `with_mut`;
   - `post` из этих контекстов → `Error::InvalidState`.
6. Poison:
   - Domain mutex → `Error::InvalidState`;
   - command lane → `Error::InvalidState`;
   - `PoisonError::into_inner()` не используется;
   - panic executor не публикует `COMMAND_SENT`.

**Gate 1:** unit-тесты command model проходят; старый production путь ещё
может компилироваться, но новый `DomainHandle::post` уже корректен.

### Phase 2 — `DomainHandle` API

1. **Не удалять** `lock()`/`with()` в этой фазе: они остаются deprecated
   до завершения миграции call sites, иначе workspace перестанет
   компилироваться.
2. Добавить explicit methods:
   - entity_put/get/meta/remove;
   - entity_snapshot;
   - entity_record_len;
   - payload_put/get;
   - register_command;
   - post;
   - dispatch_once;
   - dispatch_handle;
   - subscriber_drops/gaps/dropped_facts.
3. Все методы, кроме чистых accessor'ов, возвращают `Result` и не паникуют
   при poison.
4. Ввести `EntitySnapshot`:
   - один непрерывный `Vec<u8>`;
   - `count * (key_size + record_size)` через checked arithmetic;
   - лимит 16 KiB;
   - `try_reserve_exact`; `NoMemory` при отказе;
   - без per-entry `Box`/`to_vec`.
5. `with_mut` оставить `#[doc(hidden)]`, возвращающим `Result<R>`,
   только для adapter/device; новые call sites запрещены.

**Gate 2:** новый API и snapshot покрыты unit-тестами; workspace
компилируется; старые `lock`/`with` существуют только как deprecated
переходный путь.

### Phase 3 — Migration `automation_task.rs`

1. `reload` → `entity_snapshot(ENTITY_AUTOMATION)`.
2. `StateSource` → короткие `entity_get` через `DomainHandle`.
3. Rule evaluation выполняется без Domain lock.
4. `emit` → `DomainHandle::post`; убрать `with_mut`.
5. Стартовый reload — snapshot до запуска цикла.

**Gate 3:** automation tests зелёные; executor не под Domain lock; no
`COMMAND_SENT` на ошибках.

### Phase 4 — Migration `wifi.rs`

1. `known_records` → `entity_snapshot`.
2. `entity_put` для known/status/scan — через `DomainHandle`.
3. `store::save_domain` остаётся после Domain-операции, не под lock.
4. Убрать `with_mut`.

**Gate 4:** Wi-Fi записи persisted как раньше; Domain доступен во время
radio/store операций.

### Phase 5 — Migration `main.rs`

1. `dispatch_handle` → метод `DomainHandle`.
2. Intake:
   - command → `DomainHandle::post`;
   - simple reads/writes → explicit methods;
   - adapter/device → scoped `with_mut` только на mutation.
3. Display rebuild/navigation/scan:
   - snapshot/read API;
   - UI-работа после освобождения Domain lock.
4. Web drain:
   - per-fact `entity_get`/snapshot;
   - `MODEL`/`CLIENTS` обновляются без Domain lock.
5. System location/weather/time:
   - explicit writes;
   - event/payload publication через Domain API.
6. Automation/settings/capabilities updates:
   - explicit mutation; adapter остаётся transitional.
7. `subscriber_drops` → explicit method.

**Gate 5:** `main.rs` не использует `lock`/`with`; `with_mut` только
разрешённые adapter/device места.

### Phase 6 — Migration `zigbee_service.rs`

1. Ingest:
   - Domain reads/writes через `DomainHandle`;
   - `State` lock не удерживается во время Domain lock;
   - adapter/device mutation — scoped `with_mut` до Stage 2.
2. Register/bind/remove:
   - explicit methods или scoped `with_mut` только на adapter/device;
   - sink/store residual явно задокументирован.
3. Проверить lock order: нет Domain → command lane; нет State → Domain
   одновременно.

**Gate 6:** zigbee service tests зелёные; no deadlock in host scenarios.

### Phase 7 — Удаление старого API

1. Убедиться, что Phases 3–6 удалили все production call sites
   `DomainHandle::lock()`/`with()`.
2. Удалить `lock()` и `with()` из `DomainHandle`.
3. Убедиться, что нет обращений в `main.rs`, `zigbee_service.rs`,
   `automation_task.rs`, `wifi.rs`, tests или других crates.
4. Workspace tests должны проходить после удаления.
5. Не оставлять `lock`/`with` как «временный публичный контракт».

**Gate 7:** grep не находит `.lock()`/`.with(` на `DomainHandle`;
workspace собирается и тесты зелёные.

### Phase 8 — Static check и документация

1. Добавить проверку allowlist `.with_mut(` call sites:
   - разрешены только существующие adapter/device mutation paths;
   - запрещены `automation_task.rs`, `wifi.rs`, UI/Web, executor.
2. Проверить отсутствие `post()` внутри `with_mut`/executor.
3. Проверить отсутствие `lock`/`with` в публичном API и call sites.
4. Обновить архитектурную документацию:
   - `V3_LAYERS.md` / `V3_IMPLEMENTATION_PLAN.md` при необходимости;
   - зафиксировать, что `lock`/`with` удалены, `with_mut` переходный.
5. Зафиксировать known limitations.

**Gate 8:** static grep/check чистый; docs соответствуют коду.

### Phase 9 — Full verification

1. `cargo test -p ha-domain`
2. `cargo test -p micro-db`
3. `cargo test --workspace`
4. `tools/build-ha-app.ps1`
5. Проверить отсутствие новых `lock`/`with` call sites.
6. Проверить `with_mut` allowlist.
7. P4 не требуется до Stage 6.

## 4. Acceptance criteria

### API

- [ ] Новый API добавлен до удаления старых методов; промежуточные
      коммиты компилируются.
- [ ] В публичном API нет `DomainHandle::lock()` после Phase 7.
- [ ] В публичном API нет `DomainHandle::with()` после Phase 7.
- [ ] `EntitySnapshot` owned, имеет checked размер и failible allocation.
- [ ] `DomainHandle` методы не паникуют при poison.
- [ ] `with_mut` отсутствует в новых call sites и помечен переходным.

### Commands

- [ ] Executor не выполняется под Domain mutex.
- [ ] Command lane удерживается до публикации `COMMAND_SENT`.
- [ ] Разрешённый порядок только `command lane → Domain mutex`.
- [ ] Нет пути `Domain mutex → command lane`.
- [ ] `COMMAND_SENT` ровно один при `Ok`.
- [ ] `COMMAND_SENT` отсутствует при `Err`.
- [ ] Последовательные команды не публикуют факты в обратном порядке.
- [ ] Recursive `post` → `InvalidState` до любых mutex.
- [ ] `post` из `with_mut` → `InvalidState`.
- [ ] Panic executor → command lane poisoned; следующий `post` →
      `InvalidState`; `COMMAND_SENT` отсутствует.

### Domain state

- [ ] `entity_put`/`remove` сохраняют Stage 1c semantics.
- [ ] Storage errors не публикуют факты.
- [ ] `entity_remove(NotFound)` оставлен отдельным дефектом.
- [ ] `COMMAND_SENT` не путается с state.

### Concurrency

- [ ] Domain доступен другому потоку во время executor.
- [ ] Длительный UI/Web callback не держит Domain mutex.
- [ ] Automation evaluation не держит Domain mutex.
- [ ] Нет deadlock в тестах concurrent producer/executor/reader.
- [ ] State/DOMAIN_DB/COMMANDS не вызываются под Domain lock, кроме
      разрешённого adapter residual.

### Regression

- [ ] Существующие ha-domain/micro-db/workspace тесты зелёные.
- [ ] `build-ha-app.ps1` успешен.
- [ ] Нет изменений в `RecordSink`/`StoreSink`, RegionManager, Zigbee
      protocol, production bootstrap.

## 5. Test matrix

| Test | Ожидание |
|---|---|
| executor_runs_without_domain_lock | другой поток делает entity_get |
| executor_can_read_domain | handler читает state |
| executor_can_mutate_domain | handler делает entity_put |
| recursive_post_rejected_before_locks | InvalidState, no hang |
| post_from_with_mut_rejected | InvalidState, no hang |
| executor_error_no_command_sent | 0 фактов |
| executor_ok_one_command_sent | ровно 1 |
| two_commands_publish_in_order | факты не обгоняют друг друга |
| two_commands_serialized | нет overlap |
| executor_panic_poisons_lane | следующий post InvalidState |
| domain_poison_returns_invalid_state | без паники |
| prepare_error_no_executor | handler не вызван |
| snapshot_size_limit | >16 KiB → InvalidSize |
| snapshot_overflow | checked error |
| snapshot_empty_no_alloc | пустой snapshot |
| snapshot_allocation_failure | NoMemory (fault injection или review) |
| snapshot_no_per_entry_alloc | один data-буфер |
| long_display_web_callback | Domain не блокирован |
| concurrent_posters_readers | нет deadlock |

## 6. Suggested commit order

1. `ha-domain: add two-phase command execution`
2. `ha-domain: add explicit DomainHandle API and snapshots`
3. `ha-app: move automation off Domain lock`
4. `ha-app: move wifi reads/writes off Domain lock`
5. `ha-app: migrate main intake and UI/Web reads`
6. `ha-app: migrate zigbee service call sites`
7. `ha-domain: remove legacy DomainHandle lock and with`
8. `docs: update Stage 1d API and locking notes`

Каждый commit должен independently проходить workspace tests; production
main может временно использовать transitional `with_mut` до завершения
соответствующего phase. Удаление `lock`/`with` — только после миграции
всех call sites (Phase 7).

## 7. Stop conditions

Остановиться и запросить решение, если:

- текущие handlers нельзя перевести на `Fn + Send + Sync` без изменения
  семантики;
- требуется строгий FIFO, которого нет в утверждённой модели;
- snapshot 16 KiB недостаточен для существующих descriptors;
- adapter/device removal требует изменения `RecordSink`/`StoreSink`
  раньше Stage 2;
- обнаружен обратный порядок `Domain mutex → command lane`;
- изменение требует RegionManager/Zigbee/production wiring.

## 8. Known limitations

- Thread-local reentrant detection ловит только тот же поток. Executor
  запрещено рекурсивно вызывать `post()` и запрещено порождать поток,
  который вызывает `post()` и затем ожидается executor'ом: иначе возможна
  блокировка на command lane. Это контрактное ограничение, а не
  обнаруженный deadlock.
- Command lane сериализует выполнение, но не гарантирует строгий FIFO
  между потоками.
- `with_mut` остаётся переходным adapter/device исключением; sink может
  выполняться под Domain lock до Stage 2.
- Лимит 16 KiB действует на один `EntitySnapshot`; несколько одновременно
  существующих snapshot могут потребовать больше памяти.
- Poison command lane после panic executor не восстанавливается
  автоматически; последующие `post` → `InvalidState`.
- P4/hardware не проверяются в Stage 1d.3.

## 9. Definition of done

- Все acceptance criteria §4 выполнены.
- Полный test matrix §5 зелёный.
- Нет новых `lock`/`with`; `with_mut` только allowlist.
- Документация обновлена.
- Изменения ограничены `ha-domain` и необходимыми call sites `ha-app`/
  `zigbee-devices`/`ha-device` в рамках Stage 1d.
- Stage 1d.4 review готов; Stage 2/5 не начаты.
