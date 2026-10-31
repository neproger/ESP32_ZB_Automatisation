# Stage 1d.2 — DomainHandle и модель блокировок

> Статус: **архитектурное предложение для согласования**. Код не менялся,
> Stage 1d.3 не начат.
>
> Основание: аудит Stage 1d.1 и фактические call sites в `ha-app`, `ha-domain`,
> `zigbee-devices`, `ha-device`. Ссылки на строки относятся к текущему `main`.

## 1. Текущее состояние

### 1.1. API

`DomainHandle` (`crates/ha-domain/src/lib.rs:157-174`):

```rust
pub struct DomainHandle(Arc<StdMutex<Domain>>);

with(visit: impl FnOnce(&Domain) -> R) -> R
with_mut(visit: impl FnOnce(&mut Domain) -> R) -> R
lock() -> MutexGuard<'_, Domain>
```

- `with` и `with_mut` держат Domain mutex на весь callback.
- `lock` публично отдаёт `MutexGuard`.
- Mutex нереентерабельный; `lock().unwrap()` паникует при poison.
- `Domain::post()` (`lib.rs:490-519`) синхронно вызывает executor и только
  затем публикует `COMMAND_SENT`. Executor `Err` не публикует факт.

### 1.2. Call sites

Полный аудит: `stage1d.1` (отчёт в истории проекта). Кратко:

- `lock`: 12 вызовов в `main.rs` и `zigbee_service.rs`.
- `with`: 11 вызовов в `main.rs`, `automation_task.rs`, `wifi.rs`.
- `with_mut`: 7 вызовов в `main.rs`, `automation_task.rs`, `wifi.rs`.
- `Domain::post`: 5 вызовов, из них 3 production (`main.rs:250`,
  `main.rs:487`, `automation_task.rs:56`) и все три — под Domain mutex.
- `register_command`: 3 production вызова в `main.rs:99,106,109`.

### 1.3. Фактические проблемы

1. Executor выполняется под Domain mutex. Это связывает время работы
   `State::submit()`/Wi-Fi channel send с доступностью Domain.
2. `with_mut` позволяет выполнять произвольный код, включая executor и
   persistence sink, под Domain lock.
3. `with` используется для длительной UI/Web/automation-работы, а не только
   для короткого чтения.
4. `lock` позволяет удерживать guard вне контролируемого Domain API.
5. Отсутствие найденного обратного порядка блокировок не является
   доказательством отсутствия deadlock; нужен явный контракт.

## 2. Рассмотренные варианты

### 2.1. DomainHandle API

**A. Оставить `lock`/`with`/`with_mut` как есть.**

- Минимальные изменения.
- Сохраняет утечку mutex и выполнение внешнего кода под lock.
- Не соответствует цели Stage 1d.

**B. Механически заменить `lock()` на `with_mut()`.**

- Убирает публичный guard.
- Сохраняет ту же область блокировки и те же внешние вызовы.
- Не решает основную проблему; отклонено.

**C. Специализированные операции + переходный `with_mut`.**

- Удалить `lock` и `with` из runtime API.
- Добавить явные entity/payload/dispatch/command methods.
- `with_mut` оставить только для существующих `DomainAdapter`/`ha-device`
  транзакций, без executor/UI/Web; удалить в Stage 2/3.
- Рекомендуется.

### 2.2. Выполнение executor

**A. Оставить executor под Domain lock.**

- Сохраняет порядок и текущую семантику.
- Не решает задержки и потенциальные deadlock; отклонено.

**B. Две фазы + последовательная command lane.**

1. Под Domain lock: валидация, выбор handler, копирование args/target/meta.
2. Domain lock отпускается.
3. Command lane (отдельный mutex/очередь) сериализует выполнение handler.
4. При `Ok` Domain lock берётся снова **до освобождения command lane**,
   публикуется ровно один `COMMAND_SENT`, затем Domain lock отпускается и
   только после этого отпускается command lane. При `Err` факт не
   публикуется.

- Domain не заблокирован на время внешнего handler.
- Executor по-прежнему выполняется последовательно, как сейчас.
- Command lane удерживается до публикации, поэтому `COMMAND_SENT` не могут
  обгонять друг друга после последовательных handler.
- Допустимый порядок: **command lane → Domain mutex**. Обратный порядок
  (Domain mutex → command lane) запрещён; `post` из `with_mut` и из executor
  отклоняется до любых блокировок.
- Handler должен быть разделяемым: `Arc<dyn Fn(&[u8]) -> Result<()> + Send + Sync>`.
- Рекомендуется.

**C. Конкурентное выполнение без lane.**

- Минимальная задержка.
- Меняет сериализацию и порядок `COMMAND_SENT`, требует `Sync` handler и
  пересмотра текущих executor.
- Не рекомендуется без отдельного решения.

**D. Отдельный command worker/queue.**

- Строгий FIFO, полностью развязывает caller и executor.
- Меняет синхронный контракт `post` или требует ожидания результата.
- Больше памяти/состояния и сложная обработка ошибок; отложить.

Рекомендация: **B**, с явным ограничением: `post` из executor-а не
поддерживается (см. §7).

## 3. Сравнительная таблица

| Критерий | B: two-phase + lane | C: concurrent | D: queue/worker |
|---|---|---|---|
| Domain lock во время executor | нет | нет | нет |
| Executor сериализован | да | нет | да |
| `COMMAND_SENT` после Ok | да, lane удерживается до публикации | да, но порядок по завершению | да, FIFO |
| Caller получает executor `Err` | да | да | нужно определить |
| Реентерабельный `post` | запрещён до любых mutex | возможен | возможен при async |
| Память | O(args) | O(args) | O(queue) |
| Сложность | средняя | низкая | высокая |
| Изменение текущей семантики | минимальное | заметное | заметное |

## 4. Рекомендуемая архитектура

### 4.1. Command registry

Заменить:

```rust
Box<dyn FnMut(&[u8]) -> Result<()> + Send>
```

на разделяемый handler:

```rust
Arc<dyn Fn(&[u8]) -> Result<()> + Send + Sync>
```

Причины:

- handler должен пережить отпускание Domain lock;
- текущие `COMMAND_PROPERTY` и Wi-Fi executors уже используют
  `Arc<Mutex<State>>`/каналы и не требуют `&mut` захвата;
- если будущему handler нужно изменяемое состояние, оно должно жить за
  собственным mutex/atomic, а не возвращать выполнение под Domain lock.

`register_command` принимает `Fn`, а не `FnMut`; это изменение API нужно
отразить в call sites.

### 4.2. Две фазы команды

Подготовка под Domain lock:

- проверить `source` и `target`;
- найти handler;
- скопировать `args` в owned buffer;
- скопировать target key/meta;
- вернуть `PreparedCommand`.

Выполнение и фиксация:

- захватить command lane;
- вызвать handler без Domain lock;
- при `Err`: отпустить command lane, вернуть ошибку без факта;
- при `Ok`: **не отпуская command lane**, захватить Domain lock,
  опубликовать ровно один `COMMAND_SENT`, отпустить Domain lock, затем
  отпустить command lane;
- при ошибке захвата/poison Domain: отпустить command lane, вернуть
  `Error::InvalidState`; command уже выполнен, но факт не опубликован —
  это должно быть явно задокументировано.

Порядок блокировок: **command lane → Domain mutex**. Обратный порядок
(Domain mutex → command lane) не допускается. В планируемых call sites
command lane захватывается только после освобождения Domain mutex при
подготовке; при фиксации lane уже удерживается, затем берётся Domain mutex.
Проверка повторного входа выполняется до захвата Domain mutex, поэтому
вложенный `post` не может попасть в этот цикл.

### 4.3. Порядок

Command lane сохраняет **последовательное выполнение**. Порядок между
командами из разных потоков определяется порядком захвата lane, а не
гарантированным FIFO. Текущий Domain mutex также не даёт строгого FIFO;
поэтому это не скрытое изменение сериализации.

Command lane удерживается до публикации `COMMAND_SENT`; поэтому после
последовательного выполнения handler факты не могут быть опубликованы в
обратном порядке. Если нужен строгий FIFO поступления, требуется
queue/worker (вариант D). Это отдельное решение; в Stage 1d не
рекомендуется без требования.

### 4.4. Повторный вход

- Проверка повторного входа выполняется **в самом начале**
  `DomainHandle::post()`, до захвата Domain mutex и command lane.
- Если текущий поток уже выполняет executor, nested `post()` возвращает
  `Error::InvalidState`, не блокируясь.
- `with_mut` помечает поток как находящийся в Domain callback; `post()` из
  такого callback также возвращает `InvalidState` до любых блокировок.
- Handler может вызывать read/mutation API Domain: Domain lock свободен.
- Handler не должен вызывать `DomainHandle::post()` рекурсивно, в том числе
  из другого потока, порождённого executor: command lane нереентерабелен.
  Thread-local detection ловит только тот же поток; порождённый поток
  обязан не вызывать `post`, иначе возможна блокировка на lane. Это
  явное ограничение контракта.
- `with_mut` callback не должен вызывать `post`; попытка отклоняется
  проверкой до захвата Domain mutex.

## 5. Целевой API `DomainHandle`

```text
DomainHandle::new(Domain)

entity_put(entity, key, record, meta) -> Result<bool>
entity_get(entity, key, out) -> Result<()>
entity_meta(entity, key) -> Result<EntityMeta>
entity_remove(entity, key, meta) -> Result<()>
entity_snapshot(entity) -> Result<EntitySnapshot>
entity_record_len(entity) -> Result<usize>

payload_put(target, meta, payload) -> Result<PayloadRef>
payload_get(reference, out) -> Result<()>

register_command(type, handler) -> Result<()>  // bootstrap, короткий lock
post(type, args, target, meta) -> Result<()>   // executor вне Domain lock
dispatch_once() -> Result<usize>
dispatch_handle() -> Result<DispatchHandle>
subscriber_drops() -> Result<u64>
gaps() -> Result<u32>
dropped_facts() -> Result<u64>

with_mut(visit) -> Result<R>                    // transitional, adapter/device only
```

`EntitySnapshot` — owned копия:

```text
EntitySnapshot {
    entity: EntityType,
    key_size: usize,
    record_size: usize,
    entries: Vec<EntitySnapshotEntry>, // не больше EntityDesc.capacity
}
EntitySnapshotEntry {
    key: Box<[u8]>,     // key_size
    record: Box<[u8]>,  // record_size
}
```

Ограничения snapshot:

- размер ограничен `EntityDesc.capacity` и фиксированными
  `key_size`/`record_size`;
- `entries.capacity` резервируется заранее через fallible reservation;
  нехватка памяти → `Error::NoMemory`;
- snapshot одной таблицы не является неограниченным `Vec` по входным
  данным;
- обработка snapshot выполняется без Domain mutex; для UI/automation
  допускается несколько последовательных snapshot'ов разных таблиц.

### 5.1. Что удаляется

- `lock()` — удалить из публичного API.
- `with()` — удалить; заменить явными read-методами и snapshot.
- `entity_iter()` — убрать из публичного API. Внутренний обход Domain
  остаётся только для построения `EntitySnapshot`; callback-контракт
  «только копировать» не гарантируется технически.
- Универсальный `with_mut()` — не использовать для executor/UI/Web; оставить
  только переходно для `DomainAdapter`/`ha-device` с явным контрактом и
  удалить в Stage 2/3.

### 5.2. Почему `with_mut` остаётся временно

`DomainAdapter` и `ha-device` сейчас принимают `&mut Domain` и вызывают
`RecordSink`/`Sink` внутри своих операций. Их перевод на `DomainHandle` —
это Stage 2 (adapter cleanup) и Stage 5 (production wiring). В Stage 1d
нельзя одновременно убрать sink и переписать adapter без расширения scope.

Поэтому `with_mut` остаётся **только** для adapter/device mutation-транзакций:

- без executor;
- без UI/Web;
- без длительных циклов;
- с минимальным Domain-кодом;
- с известным остаточным риском: sink вызывается под lock до Stage 2.

Ограничение видимости технически:

- `pub(crate)` невозможен, пока `DomainAdapter`/`ha-device` находятся в
  отдельных crates;
- поэтому `with_mut` остаётся публичным, но помечается `#[doc(hidden)]` и
  не входит в документированный runtime API;
- новый код не должен вызывать `with_mut`; допустимы только существующие
  adapter/device call sites;
- удаление `with_mut` — обязательная задача Stage 2, когда adapter/device
  перейдут на `DomainHandle` и sink не будет вызываться под lock;
- для ревью добавляется статическая проверка: новые `.with_mut(` вне
  разрешённого списка не допускаются.

## 6. Locking policy

1. **Domain mutex никогда не удерживается во время вызова executor.**
2. **Domain mutex никогда не удерживается во время UI/Web-обработки.**
3. Длительные чтения выполняются через `EntitySnapshot`; публичного
   `entity_iter` нет.
4. Внешние сервисы (State, DOMAIN_DB, COMMANDS, display/web) не вызываются
   под Domain lock, кроме временного adapter/device исключения.
5. Command lane — отдельный mutex; он не заменяет Domain mutex и не даёт
   права удерживать Domain lock.
6. Domain lock захватывается только для коротких операций:
   подготовка, публикация, чтение/запись canonical state.
7. Разрешённый порядок: **command lane → Domain mutex** на фиксации
   `COMMAND_SENT`. Domain mutex не удерживается при ожидании command lane,
   State, DOMAIN_DB, COMMANDS, UI/Web locks.
8. Обратный порядок **Domain mutex → command lane** запрещён.
   `post` из `with_mut`/executor отклоняется до любых блокировок.
9. `with_mut` не вызывается из executor и не вызывает `post`.
10. Новые внешние вызовы под Domain lock запрещены; отсутствие обратного
    порядка в текущем коде не считается доказательством.

## 7. Command execution semantics

```text
DomainHandle::post
  → reentry check (before any mutex)
  → Domain lock: validate + prepare + clone handler
  → Domain unlock
  → command lane lock
  → handler(args)
  → if Err:
       command lane unlock
       return Err (no fact)
  → if Ok:
       Domain lock
         publish exactly one COMMAND_SENT
       Domain unlock
       command lane unlock
       return Ok
```

Порядок блокировок на фиксации: **command lane → Domain mutex**.  
Domain mutex никогда не удерживается при ожидании command lane.

- `Ok(())` → ровно один `COMMAND_SENT`, опубликованный до освобождения
  command lane.
- `Err` → ни одного `COMMAND_SENT`; command lane освобождается сразу.
- Последовательные команды не могут опубликовать `COMMAND_SENT` в обратном
  порядке, потому что lane удерживается до публикации.
- Commissioning/handoff не означает изменение state.
- Ошибка подготовки → `Err`, executor не вызывается.
- Ошибка фиксации после успешного executor (poison Domain) → `Err`; факт не
  публикуется; command lane освобождается; это явный наблюдаемый случай.
- Рекурсивный `post` из executor или `with_mut`: проверка выполняется до
  Domain mutex/command lane; возвращается `InvalidState`, без deadlock.
- **Panic executor:** unwind освобождает command lane, но оставляет его
  poisoned. Domain mutex при этом не удерживается. Следующий `post`
  возвращает `InvalidState`; автоматическое восстановление через
  `PoisonError::into_inner()` не выполняется. Состояние внешнего действия
  после panic считается неизвестным; `COMMAND_SENT` не публикуется.
  Command lane остаётся недоступным до перезапуска/явного решения.

## 8. UI / Web / Automation

### Display

- `rebuild`: получить snapshot групп/элементов/entities/capabilities/
  settings, отпустить Domain, строить UI по копии.
- `drain`: по факту получить нужную запись через `entity_get`/snapshot,
  обновлять UI вне Domain lock.
- `tick`/LVGL работают без Domain lock.
- `navigate` получает snapshot, а не `&Domain` под lock.

### Web

- `drain`: для каждого факта получить запись через `entity_get`; обновлять
  `MODEL`/`CLIENTS` без Domain lock.
- `flush` уже вне Domain lock.
- Web handlers не получают `DomainHandle`.

### Automation

- `reload`: snapshot `ENTITY_AUTOMATION`, загрузить engine вне Domain.
- rule evaluation: `StateSource` использует короткие `entity_get` через
  `DomainHandle` (или ограниченный state snapshot); не держит Domain lock
  на весь evaluate.
- `emit`: `DomainHandle::post`; executor вне Domain lock.
- `with_mut` не используется.

## 9. Migration plan

### `main.rs`

| Строка | Сейчас | Цель |
|---|---|---|
| 97 | `with_mut` + `register_command` | `DomainHandle::register_command` |
| 114 | `with(display::rebuild)` | snapshot + rebuild вне lock |
| 116 | `with(read_location)` | `entity_get` |
| 124 | `lock().dispatch_handle()` | `dispatch_handle()` |
| 130 | `with_mut(dispatch_once)` | `dispatch_once()` |
| 144 | `with(display::drain/tick)` | `entity_get`/snapshot + tick вне lock |
| 159 | `with(web::drain)` | per-fact `entity_get`, model вне lock |
| 172 | long `lock` по intake | явные методы; adapter/device — scoped `with_mut` только на mutation |
| 186/250/487 | `post` под lock | `DomainHandle::post` без Domain lock |
| 331/352 | display `lock` | snapshot/read API |
| 356 | system updates `lock` | explicit entity puts + system state вне Domain |
| 376 | capabilities `lock` | explicit mutation API |
| 385 | `with_mut(publish time/event)` | explicit puts/payload + `post`/publish |
| 402 | `with(subscriber_drops)` | `subscriber_drops()` |

### `zigbee_service.rs`

- 184/240 ingest: разделить Domain reads/writes и State; не держать Domain
  во время State/внешней работы; adapter/device mutation можно оставить в
  scoped `with_mut` до Stage 2.
- 321/334/358 register/bind: explicit DomainHandle methods или scoped
  `with_mut` только на adapter/device; sink residual.
- 413 remove: то же; `zigbee_remover`/store выполнять вне Domain lock, если
  возможно; иначе зафиксировать исключение до Stage 2.

### `automation_task.rs`

- 49 `with_mut` + `post` → `DomainHandle::post`.
- 87/100/115/132 `with` → snapshot/`entity_get`; evaluate вне lock.

### `wifi.rs`

- 127 `with` → `entity_snapshot`.
- 186/337/356 `with_mut` → `entity_put`.
- `store::save_domain` остаётся после Domain-операции, не под lock.

### `Domain`

- `post`/command registry перевести на `PreparedCommand` + lane.
- `register_command` принимает `Fn + Send + Sync`.
- `with`/`with_mut` не используются executor/UI/Web.
- `Domain::post` (direct) оставить только для внутренних/тестовых путей или
  удалить из production API; публичный путь — `DomainHandle::post`.

## 10. Error handling

- **Poison Domain:** `DomainHandle` внутренний lock возвращает
  `Result<MutexGuard, Error>`; все публичные методы возвращают
  `Error::InvalidState` вместо паники. `with_mut` тоже возвращает `Result`.
- **Poison command lane:** после panic executor lane poisoned. Следующий
  `post()` возвращает `Error::InvalidState`; автоматическое восстановление
  через `PoisonError::into_inner()` не выполняется. Command lane считается
  неработоспособным до перезапуска/явного решения; состояние внешнего
  действия после panic неизвестно.
- **Executor error:** вернуть исходный `Error`; `COMMAND_SENT` нет.
- **Prepare error:** вернуть `Err`; executor не вызывается.
- **Completion error:** command уже выполнен, но `COMMAND_SENT` не
  опубликован; вернуть `InvalidState` и считать это наблюдаемой ошибкой.
- **Reentrant post:** проверка до любых mutex; `InvalidState`, без deadlock.
- **Cancel/stop:** в Stage 1d не добавлять; текущий контракт не отменяет
  уже начатый executor.
- **Panic executor:** unwind освобождает command lane, но оставляет его
  poisoned; Domain lock не удерживается; `COMMAND_SENT` отсутствует.

## 11. Test plan Stage 1d.4

### Domain unit/integration

- `executor_runs_without_domain_lock`: executor блокируется на barrier;
  другой поток выполняет `entity_get` успешно.
- `executor_can_read_domain`: executor вызывает `entity_get`/`entity_put`.
- `recursive_post_returns_invalid_state`: handler вызывает `post`; без
  deadlock, `InvalidState`.
- `executor_error_publishes_no_command_sent`.
- `executor_ok_publishes_exactly_one_command_sent`.
- `two_commands_publish_in_execution_order`: первая команда удерживает
  lane до публикации; вторая не может опубликовать `COMMAND_SENT` раньше.
- `two_commands_are_serialized`: lane не допускает overlap.
- `concurrent_posters_and_readers`: нет deadlock; Domain доступен во время
  executor.
- `poisoned_domain_returns_invalid_state`: без паники.
- `executor_panic_poisons_command_lane`: panic ловится тестом; следующий
  `post()` возвращает `InvalidState`; `COMMAND_SENT` отсутствует.
- `recursive_post_rejected_before_locks`: barrier доказывает, что вложенный
  вызов не ждёт Domain/command lane.
- `post_from_with_mut_returns_invalid_state`: без deadlock.
- `prepare_error_does_not_call_executor`.

### UI/Web/automation

- snapshot read не держит Domain во время обработки barrier.
- длительный `display`/`web` callback не блокирует producer.
- automation reload/evaluate не держит Domain во время engine работы.
- `entity_snapshot` ограничен capacity; недостаток памяти → `NoMemory`.

### Concurrency/regression

- текущие `ha-domain`, `micro-db`, workspace тесты зелёные.
- `COMMAND_SENT` остаётся audit-фактом, не state.
- `entity_put`/remove storage errors не публикуют факты (Stage 1c).
- `entity_remove(NotFound)` остаётся отдельным дефектом.
- P4 не требуется до Stage 6.

## 12. Ограничения и риски

- `with_mut` временно сохраняется для adapter/device; sink может выполняться
  под Domain lock до Stage 2. Технически `pub(crate)` невозможен без
  переноса adapter/device; используется `#[doc(hidden)]` + запрет новых
  call sites.
- Command lane сохраняет последовательность, но не гарантирует строгий FIFO
  между потоками.
- Рекурсивный `post` запрещён; thread-local ловит только тот же поток.
  `post` из потока, порождённого executor, может заблокироваться на lane;
  это явное ограничение. Для полноценной реентерабельности нужен
  queue/worker.
- Handler должен быть `Fn + Send + Sync`; изменяемое состояние — за
  собственным mutex.
- Snapshot для UI/automation требует памяти; размер ограничен capacity и
  fallible reservation, но несколько snapshot'ов могут временно занимать
  память одновременно.
- `COMMAND_SENT` публикуется до освобождения command lane, но после
  внешнего side effect executor'а; это audit-факт, не state.
- Panic executor оставляет command lane poisoned; последующие `post`
  возвращают `InvalidState`, автовосстановления нет.
- Удаление `with`/`lock` затрагивает много call sites; миграция должна быть
  поэтапной и с тестами.
- `DomainAdapter`, `ha-app`, production wiring и `RegionManager` не входят
  в Stage 1d.3 без отдельного согласования.
- P4 не проверяется.

## 13. Что требуется утвердить куратору

1. Command model: **two-phase + serialized command lane**, handler
   `Arc<dyn Fn + Send + Sync>`, рекурсивный `post` → `InvalidState`.
2. Command lane удерживается до публикации `COMMAND_SENT`; допустимый
   порядок **command lane → Domain mutex**, обратный запрещён.
3. Reentrant detection выполняется до захвата Domain mutex и command lane;
   `post` из `with_mut` запрещён.
4. Poison policy: Domain и command lane → `Error::InvalidState`, без паники;
   command lane после panic не восстанавливается автоматически.
5. Целевой `DomainHandle` API: удалить `lock`/`with`/`entity_iter`; добавить
   explicit methods и bounded `EntitySnapshot`; `with_mut` — только
   переходный adapter/device, `#[doc(hidden)]`, без новых call sites.
6. Порядок команд: сериализация через lane; строгий FIFO не гарантируется.
7. Разрешить Stage 1d.3 только после утверждения этого документа.

## 14. Изменения после архитектурного ревью

- `COMMAND_SENT` теперь публикуется до освобождения command lane; добавлен
  допустимый порядок **command lane → Domain mutex** и запрет обратного.
- Reentrant-проверка перенесена в начало `post()`, до Domain mutex и
  command lane; `post` из `with_mut` запрещён.
- Определён poison command lane: последующие `post()` → `InvalidState`,
  автоматическое восстановление запрещено; panic executor не публикует
  `COMMAND_SENT`.
- `with_mut` зафиксирован как `#[doc(hidden)]` переходный API только для
  adapter/device; новые call sites запрещены; удаление — Stage 2.
- `entity_iter` удалён из целевого API; введён bounded `EntitySnapshot`
  с fallible reservation и ограничением по capacity.
- Добавлены тесты порядка `COMMAND_SENT`, reentrant-проверки до mutex,
  panic/poison command lane и доступности Domain во время executor.
