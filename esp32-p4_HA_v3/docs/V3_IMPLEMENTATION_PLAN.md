# HA V3 — план восстановления архитектуры V2

> Статус: **утверждённое направление, этап 1 требует закрытия открытых вопросов**.
>
> HA V3 = HA V2 Architecture + Rust implementation. V2 остаётся неизменяемым
> архитектурным эталоном. V3 не добавляет новые слои и не «улучшает» владение
> данными ради удобства Rust.
>
> Эталон: `esp32-p4_HA_v2/docs/ARCHITECTURE.md` и контракты
> `domain/`, `services/`, `clients/`, `storage/`.
>
> Связанные документы:
> - [V3_LAYERS.md](V3_LAYERS.md) — архитектурный контракт V3;
> - [V2_TO_V3_MAPPING.md](V2_TO_V3_MAPPING.md) — аудит текущего состояния;
> - [TARGET_ARCHITECTURE.md](TARGET_ARCHITECTURE.md) — canonical-модель.

## 1. Правила выполнения плана

1. **V2 первичен.** Если V3-код расходится с V2-контрактом, код считается
   неправильным до явно оформленного платформенного исключения.
2. **Не добавлять функции V3 до завершения этапа 6.** Сначала восстанавливается
   владение и границы V2.
3. **Domain владеет canonical-записями и политикой persistence.**
   `micro-db` — только механизм хранения: таблицы, слоты, индексы, RAM/Flash.
4. **Адаптеры и сервисы не пишут canonical-записи в storage в обход Domain.**
   Ни `RecordSink`, ни `StoreSink`, ни прямой вызов `store::save_domain` не
   являются допустимой границей.
5. **Порядок работ фиксирован:** Domain → Zigbee adapter → сервисы → runtime →
   `main.rs` → сценарии V2. Нельзя переписывать `main.rs` раньше Domain.
6. **`main.rs` и `DomainAdapter` не меняются на этапе 1.** Их рефакторинг
   начинается только после прохождения контрактных тестов Domain.

## 2. Этапы

| Этап | Работа | Результат |
|---|---|---|
| 1 | Domain API + persistence | Domain — единственный владелец canonical state и политики хранения |
| 2 | Zigbee adapter | `DomainAdapter` ограничен Zigbee-проекцией и Zigbee-private lifecycle |
| 3 | System / Automation / Settings / Display | Каждый сервис сам выполняет свой сценарий и пишет через Domain API |
| 4 | Runtime и intake | Нет центрального application controller; inbox принадлежит сервису |
| 5 | `main.rs` | Только composition root и bootstrap |
| 6 | V2 scenario verification | Поведение подтверждено по сценариям V2, а не только unit-тестами |

## 3. Этап 1 — Domain API + persistence

### 3.1. Граница владения

```text
composition root
  └── создаёт низкоуровневые ресурсы micro-db
        └── передаёт их Domain при инициализации

Domain
  ├── descriptors canonical-сущностей
  ├── Backing policy
  ├── RAM/Flash mutation
  ├── restore
  ├── Journal / Dispatcher
  └── command registry

service / adapter
  └── вызывает только Domain API
```

- Composition root создаёт инфраструктуру хранения.
- Domain владеет persistence policy и решает, какая сущность и когда пишется.
- Сервисы не получают доступа к persistent tables и `micro-db` API.
- `micro-db` не знает про Domain, Zigbee, Web, Automation.

### 3.2. Публичный контракт Domain

```text
Domain::new(config, clock, storage)
Domain::register_entity(EntityDesc)
Domain::restore() -> Result<RestoreReport>

Domain::entity_put(entity, key, record, meta) -> Result<bool>
Domain::entity_get(entity, key, out) -> Result<()>
Domain::entity_meta(entity, key) -> Result<EntityMeta>
Domain::entity_remove(entity, key, meta) -> Result<bool>
Domain::entity_iter(entity, visit) -> Result<()>

Domain::payload_put(target, meta, payload) -> Result<PayloadRef>
Domain::payload_get(reference, out) -> Result<()>

Domain::register_command(type, executor)
Domain::post(type, args, target, meta) -> Result<()>

Domain::subscribe(filter, sink)
Domain::dispatch_once()
```

Внутренние, не публичные для сервисов:

```text
StorageBackend
PersistedTable / RAM Table
Backing::{Ram, Flash, RamFlash}
persist_key
restore policy
storage error mapping
```

### 3.3. Backing

Текущая таблица V2-политики, которую нужно сохранить:

| Entity | V2 Backing | V2 persist key |
|---|---|---|
| Property State | RAM | — |
| Capabilities | RAM | — |
| Entity | RAM | — |
| Logical Device | RAM | — |
| Weather | RAM | — |
| Wi-Fi Scan | RAM | — |
| Wi-Fi Status | RAM | — |
| Automation | RAM + Flash | `automation_v5` |
| Location | RAM + Flash | `location` |
| Group | RAM + Flash | `group` |
| Group Item | RAM + Flash | `group_item_v3` |
| Group Automation | RAM + Flash | `group_auto_v1` |
| Wi-Fi Known | RAM + Flash | `wifi_known` |
| Settings | RAM + Flash | `settings` |

Обязательное поведение:

| Backing | Чтение | Запись | Restore |
|---|---|---|---|
| `Ram` | RAM | RAM | нет |
| `Flash` | по контракту V2 | Flash | да |
| `RamFlash` | RAM | RAM + Flash | да |

**Открытый вопрос 1:** точная семантика `Flash` в V2 — только Flash или
Flash-backed таблица с RAM-кэшем. До ответа `Backing::Flash` не реализуется
«на глаз».

**Открытый вопрос 2:** порядок согласованной RAM/Flash mutation и поведение при
ошибке между двумя представлениями. Требуемый инвариант: ошибка Flash не должна
оставлять Domain в состоянии, противоречащем persistence. Конкретный порядок
(flash → RAM или RAM → flash с rollback) должен быть взят из V2/micro-db
семантики, а не выбран произвольно.

### 3.4. `entity_put` / `entity_remove`

- `entity_put` возвращает `changed`.
- `entity_remove` возвращает `bool`: `true` — запись удалена, `false` — записи
  не было.
- При `changed=false` не выполняется запись во Flash и не публикуется факт.
- При `Ok(false)` для remove не выполняется запись во Flash и не публикуется
  `ENTITY_REMOVED`.
- При ошибке storage:
  - возвращается ошибка;
  - факт не публикуется;
  - RAM-представление не должно оказаться изменённым сильнее, чем позволяет
    выбранный V2-порядок mutation/rollback.
- `ENTITY_REMOVED` несёт полный canonical key.

### 3.5. Restore

`restore` — lifecycle-операция Domain, а не пользовательская мутация:

- выполняется до запуска подписчиков;
- не вызывает публичный `entity_put` как обычный сервис;
- не вызывает Web/Display/UI;
- проверяет descriptor, размеры, ключ, backing и `persist_key`;
- различает:
  - неизвестный `persist_key`;
  - неизвестную версию схемы;
  - несовместимый размер записи;
  - некорректный ключ;
  - повреждённую запись;
  - запись без зарегистрированного descriptor.
- повреждение данных и несовместимость версии — разные ошибки;
- `RestoreReport` отражает факт восстановления, но не скрывает ошибку;
- источник (`source`) восстанавливается по правилам V2 или сохраняется в
  persistent record. Восстанавливать все записи как `SOURCE_ZIGBEE` запрещено.

### 3.6. DomainHandle

`DomainHandle` — cloneable handle Domain API:

```text
domain.entity_put(...)?
domain.entity_get(...)?
domain.entity_remove(...)?
domain.post(...)?
```

Запрещено:

- `lock() -> MutexGuard<Domain>`;
- `with_mut(...)`;
- любой другой способ получить изменяемый доступ к внутреннему Domain.

### 3.7. `Domain::post` и executor

1. Domain находит executor.
2. Domain готовит вызов.
3. Domain отпускает внутреннюю блокировку.
4. Вызывается executor.
5. При успешном handoff пишется `COMMAND_SENT`.

Успешный handoff = executor принял команду и вернул `Ok`; это не означает
выполнение команды и не означает подтверждение устройства. Ошибка executor’а
не пишет `COMMAND_SENT`. Executor не вызывается под внутренним Domain-lock.

### 3.8. Контрактные тесты этапа 1

- RAM-only entity никогда не попадает во Flash.
- `RamFlash` переживает reboot и восстанавливается.
- `PROPERTY_STATE` не восстанавливается.
- Ошибка Flash возвращается из `entity_put`/`entity_remove`.
- Ошибка Flash не публикует факт.
- Повторный `entity_put` с теми же данными не пишет во Flash.
- `entity_remove` отсутствующей записи возвращает `false`, не пишет во Flash и
  не публикует факт.
- После restore canonical records совпадают по key/record, `source` и metadata
  соответствуют V2.
- Несовместимая и повреждённая запись дают явную ошибку, а не тихий skip.
- `DomainHandle` не отдаёт `MutexGuard`/изменяемый доступ.
- Executor не вызывается под Domain-lock.
- Существующие `ha-domain` тесты остаются зелёными.

### 3.9. Вне этапа 1

Не менять:

- `main.rs`;
- `DomainAdapter` (кроме подготовки к следующему этапу после тестов);
- `RecordSink`/`StoreSink`;
- System/Display/Automation/Web;
- Zigbee command semantics, кроме проверки, что Domain API не ломает текущий путь.

## 4. Этап 2 — Zigbee adapter

После этапа 1:

- `DomainAdapter` регистрирует только Zigbee-проекцию, capabilities, state и
  Zigbee-private lifecycle;
- `RecordSink`, `put_record`, generic CRUD, Device lifecycle и persistence
  удаляются из adapter’а;
- Device service остаётся владельцем Logical Device/Entity lifecycle;
- Zigbee adapter регистрирует remover, но не пишет canonical storage.

## 5. Этап 3 — сервисы

- System сам пишет Location/Weather/время и публикует события.
- Automation сам меняет enabled/record layout; `record[384]` уходит из `main`.
- Settings — через Domain API/профильный сервис.
- Display сам обрабатывает навигацию/scan UI.
- Web/Display/Automation не используют `main` как посредника для команд.

## 6. Этап 4 — runtime/intake

- Убрать `intake` как центральный application controller.
- У каждого сервиса свой bounded inbox; polling допустим только внутри сервиса,
  который владеет inbox.
- Убрать `system::take`, `display::take_nav`, `capabilities::take` из main-цикла.
- `main` не опрашивает Wi-Fi и не публикует системные события за сервисы.

## 7. Этап 5 — `main.rs`

Разрешено:

1. ESP-IDF/log init.
2. Создание Domain и storage infrastructure.
3. Регистрация descriptors/capacities/commands.
4. `Domain::restore`.
5. Создание сервисов и их зависимостей.
6. Регистрация подписчиков.
7. Запуск задач.
8. Bootstrap end.

Запрещено:

- `match` по пользовательским CRUD-запросам;
- Automation/Settings/Location/Weather логика;
- Device removal logic;
- Display navigation;
- Wi-Fi polling;
- публикация системных событий;
- изменение canonical records.

## 8. Этап 6 — V2 scenarios

Проверить сценарии V2 на железе и host-тестах:

- первый старт и restore;
- Zigbee report → state → delta;
- command → executor handoff → `COMMAND_SENT`;
- device interview/configure/remove/leave;
- Automation time/state/event → command;
- System time/weather → Domain;
- Wi-Fi scan/connect/known/autostart;
- Web snapshot/delta/slow client;
- Display navigation and state updates;
- persistence policy RAM/Flash/RamFlash;
- reboot after every persistent mutation;
- storage error and corrupt record behavior.

## 9. Definition of done

Этап 1 завершён, когда:

- Domain descriptors выражают backing и `persist_key`;
- Domain сам выполняет RAM/Flash mutation и restore;
- `entity_remove` возвращает `bool`;
- storage errors не публикуют факты;
- `DomainHandle` не отдаёт изменяемый Domain;
- `post` вызывает executor вне Domain-lock;
- все контрактные тесты зелёные;
- `RecordSink` ещё может существовать как переходный код, но **ни один
  production-путь не использует его для persistence**.

Только после этого начинается этап 2.
