# HA V3 — план восстановления архитектуры V2

> Статус: **утверждённое направление; этап 0 зафиксирован и готов к работе**.
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
5. **Порядок работ фиксирован:** `micro-db` → Domain → Zigbee adapter → сервисы →
   runtime → `main.rs` → сценарии V2. Нельзя переписывать `main.rs` раньше Domain.
6. **`main.rs` и `DomainAdapter` не меняются на этапах 0 и 1.** Их рефакторинг
   начинается только после прохождения контрактных тестов Domain.
7. **Host-first.** Работа и приёмка этапов 0–5 идут на host-тестах. Держать P4 в
   рабочем состоянии и тратить время на его сборку/отладку на каждом шаге не
   нужно: в ходе миграции будут сильные изменения, и прошивка может не собираться
   и не работать на плате. Обкатка на P4 и проверка сценариев V2 — этап 6; только
   после него плата снова обязана работать.
8. **Canonical ABI — внешний контракт.** Типы, числовые id, layout ключей и
   записей живут в `ha-model` (V2 `RECORD_MODEL.md`, `ha_model/*.h`) и не меняются
   ни одним этапом. Сервисы не интерпретируют запись по magic-офсетам. Изменение
   layout — новый тип или версия `persist_key`, а не правка записи. `micro-db`
   получает `key_size`/`payload_size` из descriptor и хранит payload как
   opaque-байты.

## 2. Этапы

| Этап | Работа | Результат |
|---|---|---|
| 0 | `micro-db`: инверсия Engine/Storage | `Table` — единый Engine, RAM/Flash — подчинённые Storage Backend; формат Flash не меняется |
| 1 | Domain API + persistence | Domain — единственный владелец canonical state и политики хранения |
| 2 | Zigbee adapter | `DomainAdapter` ограничен Zigbee-проекцией и Zigbee-private lifecycle |
| 3 | System / Automation / Settings / Display | Каждый сервис сам выполняет свой сценарий и пишет через Domain API |
| 4 | Runtime и intake | Нет центрального application controller; inbox принадлежит сервису |
| 5 | `main.rs` | Только composition root и bootstrap |
| 6 | V2 scenario verification | Поведение подтверждено на P4 и host-тестах по сценариям V2 |

## 3. Этап 0 — micro-db: инверсия Engine/Storage

Цель: вернуть границу V2 внутри `micro-db`, не меняя поведение Domain.
Этап не добавляет функций и не меняет persistence policy; он делает `Table`
единым Engine, а RAM/Flash — подчинёнными Storage Backend.

Запрещено на этапе 0: менять `main.rs`, `DomainAdapter`, сервисы, формат
journal/flash, раскладку разделов и публичное поведение `ha-domain`.

Проверка этапа — host-тесты; сборка и запуск на P4 не требуются (правило 7).

### 3.1. Целевая структура

```text
Table<S: Storage>          engine: geometry, slot lifecycle, hash index,
  │                        free-list, generation/version, live count
  ├── Ram                  непрерывный массив слотов
  ├── Flash                существующий FlashTable (формат, recovery, compaction)
  └── RamFlash             write-through: Flash commit, затем RAM

Ring<T>                    без изменений, не входит в Engine
```

- `Table` не хранит слоты, ключи, payload или meta.
- `Storage` — единственный владелец canonical slot state `meta + key + payload`.
- `index`, `free`, `live` — производные структуры, пересобираются при `open`
  (V2 `mstore_runtime_rebuild`).
- Engine меняет производное состояние только после успешного `write_*`.

### 3.2. Контракт Storage (перенос V2 `mstore_storage.h`)

| Операция | Rust | V2 |
|---|---|---|
| `read_meta(slot)` | `&self` | `mstore_storage.h:34` |
| `read_key(slot, out)` | `&self` | `:36` |
| `read_slot(slot, key, payload)` | `&self` | `:38` |
| `write_slot(slot, meta, key, payload)` | `&mut self` | `:40` |
| `write_meta(slot, meta)` | `&mut self` | `:42` |
| `clear_all()` | `&mut self` | `:44` |
| `sync()` | `&mut self` | `:45` |

`close` не переносится — в Rust его заменяет `Drop`. `sync` остаётся в трейте как
backend-lifecycle с no-op реализацией и никогда не вызывается из `Table`.

`read_key` — операция V2, а не добавление: `slot_find` сравнивает probing-цепочку
с canonical `slot.key`, не читая payload (`MSTORE.md` §2.9).

Чтения объявлены `&self` по контракту V2 (`const mstore_storage_t *`). Чтобы
`FlashStorage` мог это выполнить, на этапе 0 меняется локальный trait:
`NorFlash::read(&mut self)` → `read(&self)`. Это изменение этапа 0, а не
констатация текущего состояния. Внешних `embedded-storage`/`nor-flash` в проекте
нет; все три реализации (`CFlash`, `SimFlash`, `CrashFlash`) допускают такую
сигнатуру без `RefCell` и синхронизации.

Причина обязательности `&self`: публичные чтения Domain (`entity_get`,
`entity_meta`, `entity_iter`) вызываются через `DomainHandle::with`
(`automation_task.rs`, `main.rs`, `wifi.rs`, `display`, `web`). Смена Storage на
`&mut` потребовала бы менять сервисы, что этапом 0 запрещено.

### 3.3. Слот и владение

```text
slot = meta { used, generation, version } + key[key_size] + payload[payload_size]
```

- Геометрия задаётся рантаймом: `Geometry { capacity, key_size, payload_size }`.
  Отдельных типов под каждую геометрию и `Fixed` больше нет.
- `Ram` — один `Vec<u8>` на `capacity * slot_len`, без выделений на слот.
- `Flash` — `FlashTable` как есть: журнал «слот → последняя запись», `latest[]`
  остаётся приватной директорией backend'а, а не вторым индексом.
- Tombstone (`write_meta` с `used=false`) — часть контракта Storage.
- Итерация — `for_each(slot, meta, key, payload)`; `&self`-чтение flash идёт
  через scratch-буфер backend'а.

### 3.4. RamFlash

`write_slot`/`write_meta`: сначала Flash commit, затем RAM mutation. Ошибка Flash
возвращается как есть, RAM не изменяется; откат не нужен по построению.
Текущая схема `RAM → append → rollback` из `persisted.rs` удаляется вместе с
`PersistedTable` как архитектурной сущностью.

### 3.5. Совместимость Flash

Формат записи, заголовок, CRC, commit-marker, recovery и compaction не меняются.
`latest[]` и правило last-wins при восстановлении сохраняются.

Этап 0 не меняет раскладку разделов и `persist_id`. Совместимость фиксируется
host-тестом: записи, созданные текущим форматом (`ha_zigbee_devices`,
`ha_domain`), открываются и читаются новым Engine на `SimFlash`. Переход Domain
на per-entity таблицы — этап 1, вместе с решением о миграции/erase.

### 3.6. Что разрешено менять

- `crates/micro-db/*`;
- `crates/ha-domain` — механическая адаптация к байтовому `Table`/`Storage`;
  публичный контракт Domain не меняется;
- `crates/ha-app/src/store.rs`;
- `crates/zigbee-devices/src/registry.rs` — потребитель `micro-db`, не `DomainAdapter`.

### 3.7. Контрактные тесты этапа 0

Существующие сценарии `table.rs`/`flash.rs`/`persisted.rs` сохраняются
(переписывается вызов, не смысл). Добавляются:

- повторная запись слота: одна запись в журнал, `version` растёт только при
  реальном изменении;
- tombstone переживает reopen и compaction; `read_meta` после него `used=false`;
- `clear_all` очищает RAM и Flash;
- ошибка Flash в `RamFlash` → `Err`, RAM совпадает с последним commit;
- reopen восстанавливает `index`/`free`/`live` ровно по слотам;
- рост `capacity` без переформатирования сохраняет все записи;
- байтовая совместимость: формат `ha_zigbee_devices` с прежними
  `geometry`/`persist_id` открывается и читается на `SimFlash`;
- payload проходит Storage байт-в-байт: `Value` ABI (8 байт) и canonical-байты не
  меняются;
- Domain read-API остаётся `&self` (`entity_get`/`entity_meta`/`entity_iter`).

### 3.8. Вне этапа 0

- `Backing::Flash` (flash-only) — политикой V2 не используется, реализуется
  только при появлении реального потребителя;
- Region Adapter и per-entity таблицы;
- persistence policy Domain, restore, `source`;
- `entity_remove`/`Result<bool>`;
- zero-copy `acquire/release` — RAM-only оптимизация по результатам замеров.

### 3.9. Definition of done этапа 0

- `Table` не содержит слотов; слоты принадлежат `Storage`;
- `Storage` реализует семь операций V2; `NorFlash::read` — `&self`;
- `PersistedTable` и `Fixed` удалены;
- `RamFlash` пишет Flash до RAM и не имеет rollback-ветки;
- формат Flash не изменился; host-тест подтверждает чтение записей, созданных
  прежним форматом;
- все тесты этапа 0 и существующие тесты `ha-domain` зелёные;
- `main.rs`, `DomainAdapter`, сервисы не изменены.

## 4. Этап 1 — Domain API + persistence

### 4.1. Граница владения

```text
composition root
  └── создаёт низкоуровневые ресурсы micro-db
        └── передаёт их Domain при инициализации

Domain
  ├── descriptors canonical-сущностей
  ├── Backing policy
  ├── mutation через micro-db Storage
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

### 4.2. Публичный контракт Domain

```text
Domain::new(journal, payload, payload_max)          — RAM-only (тесты, текущий main)
Domain::new_with_storage(config, clock, storage)    — с persistence
Domain::register_entity(EntityDesc)
Domain::restore() -> Result<RestoreReport>

Domain::entity_put(entity, key, record, meta) -> Result<bool>
Domain::entity_get(entity, key, out) -> Result<()>
Domain::entity_meta(entity, key) -> Result<EntityMeta>
Domain::entity_remove(entity, key, meta) -> Result<()>
Domain::entity_iter(entity, visit) -> Result<()>

Domain::payload_put(target, meta, payload) -> Result<PayloadRef>
Domain::payload_get(reference, out) -> Result<()>

Domain::register_command(type, executor)
Domain::post(type, args, target, meta) -> Result<()>

Domain::subscribe(filter, sink)
Domain::dispatch_once()
```

`Domain::new` остаётся для тестов и текущего `main.rs`. `new_with_storage` и
`restore` вызываются composition root на этапе 5; до этого `main.rs` не меняется
(правило 6).

`EntityDesc.key_size`/`record_size` — производные canonical-констант (§4.3), а не
самостоятельные параметры хранения.

Внутренние, не публичные для сервисов:

```text
StorageBackend
Table<S> / Storage::{Ram, Flash, RamFlash}
Backing::{Ram, Flash, RamFlash}
persist_key
restore policy
storage error mapping
```

### 4.3. Canonical ABI и геометрия

Источник истины — `ha-model` (V2 `RECORD_MODEL.md`, `ha_model/*.h`): числовые id,
layout ключей и записей. `key_size`, `payload_size` и `capacity` — производные от
протокола и лимитов bootstrap, а не самостоятельные параметры хранения.
Descriptor собирается из canonical-констант; `micro-db` получает готовую
геометрию.

- `micro-db` хранит payload как opaque-байты и не интерпретирует canonical-типы.
- Изменение layout — новый тип или версия `persist_key`, а не правка записи
  (V2 `RECORD_MODEL.md` §3, §4).
- Размер протокольной формы и физической записи совпадает только там, где
  zero-marshalling гарантирован (V2 §6); это проверяется тестом, а не
  предполагается.
- Сервисы не читают и не пишут запись по magic-офсетам. Сейчас это нарушено:
  `ha-automation` держит свой `RULE_RECORD_LEN = 456` и офсеты 384–387,
  `main.rs:216` пишет `record[384]`, `display.rs:637,663` разбирает
  `group`/`group_item` по смещениям.

Задачи этапа 1:

- перенести в `ha-model` раскладки `ha_rule`, `ha_group`, `ha_group_item` с
  compile-time проверками размеров и офсетов (как `static_assert` в V2);
- удалить `RULE_RECORD_LEN` из `ha-automation`, использовать
  `canonical::AUTOMATION_RECORD_LEN`;
- `ha-domain` не является точкой импорта canonical для новых зависимостей:
  единственный источник — `ha-model`.

Потребители раскладок (`main.rs`, `display`, `ha-automation`) переключаются на
canonical-структуры в этапе 3 (правило 6).

Совместимость с V2 не считается доказанной, пока не пройдёт тест сравнения
layout'ов V2 и V3 (§4.9), в том числе `ha_rule_t.enabled` на смещении 384.
Значения для сравнения берутся из V2-заголовков и `static_assert`, а не из V3-кода.

### 4.4. Backing

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
| `RamFlash` | RAM | Flash commit, затем RAM | да |
| `Flash` | — | — | — |

`Backing::Flash` не используется ни одним дескриптором V2; регистрация такой
сущности отклоняется (этап 0, §3.8). Реализация — только при появлении реального
потребителя.

Механизм mutation/restore даёт `micro-db` (этап 0): `RamFlash` пишет Flash до RAM,
ошибка Flash возвращает `Err` и не меняет RAM; `restore` — открытие таблицы при
регистрации, без публикации фактов (§4.6).

Переход на per-entity таблицы меняет flash layout. Судьба текущего раздела
`ha_domain` (erase либо миграция чтением старой таблицы) фиксируется в этапе 1 до
реализации; прецедент — разовый erase старого ABI в V2 (A7).

### 4.5. `entity_put` / `entity_remove`

- `entity_put` возвращает `changed`.
- `entity_remove` следует V2: `Result<()>`, отсутствие записи — `Err(NotFound)`.
  `bool` нужен только адаптеру, и он уже маппит `NotFound` сам.
- При `changed=false` не выполняется запись во Flash и не публикуется факт.
- При `Err(NotFound)` для remove не выполняется запись во Flash и не публикуется
  `ENTITY_REMOVED`.
- При ошибке storage:
  - возвращается ошибка;
  - факт не публикуется;
  - RAM совпадает с последним commit (инвариант `RamFlash`, этап 0).
- `ENTITY_REMOVED` несёт полный canonical key.

### 4.6. Restore

`restore` — lifecycle-операция Domain, а не пользовательская мутация. В V2 её
отдельного вызова нет: восстановление происходит при регистрации таблицы
(`mstore_table_init → mstore_storage_open → mstore_runtime_rebuild`).

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
- восстановление не публикует факты, поэтому `source` в нём не участвует:
  `source` — поле факта, а не persisted-записи. Записывать восстановленные
  записи как `SOURCE_ZIGBEE` запрещено.

### 4.7. DomainHandle

`DomainHandle` — cloneable handle Domain API:

```text
domain.entity_put(...)?
domain.entity_get(...)?
domain.entity_remove(...)?
domain.post(...)?
```

Запрещено:

- `lock() -> MutexGuard<Domain>` — с этапа 1;
- любой другой способ получить изменяемый доступ к внутреннему Domain.

`with_mut` на этапе 1 остаётся для неизменяемых потребителей (`main.rs`,
`DomainAdapter`, сервисы) и удаляется на этапе 3–4, когда каждый сервис получает
свой API. Убрать его на этапе 1 нельзя: это потребует правок там, где правило 6
их запрещает.

### 4.8. `Domain::post` и executor

1. Domain находит executor.
2. Domain готовит вызов.
3. Domain отпускает внутреннюю блокировку.
4. Вызывается executor.
5. При успешном handoff пишется `COMMAND_SENT`.

Успешный handoff = executor принял команду и вернул `Ok`; это не означает
выполнение команды и не означает подтверждение устройства. Ошибка executor’а
не пишет `COMMAND_SENT`. Executor не вызывается под внутренним Domain-lock.

### 4.9. Контрактные тесты этапа 1

Все тесты этапа — host; P4 не требуется (правило 7).

- RAM-only entity никогда не попадает во Flash.
- `RamFlash` переживает reboot на `SimFlash` и восстанавливается.
- `PROPERTY_STATE` не восстанавливается.
- Ошибка Flash возвращается из `entity_put`/`entity_remove`.
- Ошибка Flash не публикует факт.
- Повторный `entity_put` с теми же данными не пишет во Flash.
- `entity_remove` отсутствующей записи возвращает `NotFound`, не пишет во Flash и
  не публикует факт.
- После restore canonical records и `version` совпадают; факты не публикуются,
  `source` не участвует.
- Несовместимая и повреждённая запись дают явную ошибку, а не тихий skip.
- `DomainHandle` не отдаёт `MutexGuard`; `with_mut` остаётся только до этапа 3.
- Executor не вызывается под Domain-lock.
- Для каждой сущности `EntityDesc.key_size`/`record_size` совпадают с
  `canonical::*_KEY_LEN`/`*_RECORD_LEN`.
- Canonical-раскладки `ha_rule`, `ha_group`, `ha_group_item` проходят
  compile-time проверки размеров и офсетов.
- Layout-паритет V2↔V3: тест сравнивает размеры и офсеты `ha_rule`, `ha_group`,
  `ha_group_item` со значениями V2 (`ha_model/*.h`), включая `ha_rule_t.enabled`
  на смещении 384. До зелёного теста совместимость не считается доказанной.
- Существующие `ha-domain` тесты остаются зелёными.

### 4.10. Вне этапа 1

Не менять:

- `main.rs`;
- `DomainAdapter` (кроме подготовки к следующему этапу после тестов);
- `RecordSink`/`StoreSink`;
- System/Display/Automation/Web;
- потребителей canonical-раскладок (`main.rs`, `display`, `ha-automation`) —
  их перевод на структуры `ha-model` выполняется в этапе 3;
- Zigbee command semantics, кроме проверки, что Domain API не ломает текущий путь.

## 5. Этап 2 — Zigbee adapter

После этапа 1:

- `DomainAdapter` регистрирует только Zigbee-проекцию, capabilities, state и
  Zigbee-private lifecycle;
- `RecordSink`, `put_record`, generic CRUD, Device lifecycle и persistence
  удаляются из adapter’а;
- Device service остаётся владельцем Logical Device/Entity lifecycle;
- Zigbee adapter регистрирует remover, но не пишет canonical storage;
- после этапа 2 ни один production-путь не использует `RecordSink`/`StoreSink`
  для persistence. Проверка — host-тесты.

## 6. Этап 3 — сервисы

- System сам пишет Location/Weather/время и публикует события.
- Automation сам меняет enabled/record layout; `record[384]` уходит из `main`;
  `ha-automation` использует canonical-раскладку и `AUTOMATION_RECORD_LEN`.
- Display разбирает `group`/`group_item` через canonical-структуры `ha-model`, а не
  по magic-офсетам.
- Settings — через Domain API/профильный сервис.
- Display сам обрабатывает навигацию/scan UI.
- Web/Display/Automation не используют `main` как посредника для команд.

## 7. Этап 4 — runtime/intake

- Убрать `intake` как центральный application controller.
- У каждого сервиса свой bounded inbox; polling допустим только внутри сервиса,
  который владеет inbox.
- Убрать `system::take`, `display::take_nav`, `capabilities::take` из main-цикла.
- `main` не опрашивает Wi-Fi и не публикует системные события за сервисы.

## 8. Этап 5 — `main.rs`

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

## 9. Этап 6 — V2 scenarios

Единственный этап, где P4 обязателен. До него плата может не собираться и не
работать (правило 7); после него прошивка снова обязана работать на P4.

Проверить сценарии V2 на P4 и host-тестах:

- первый старт и restore;
- Zigbee report → state → delta;
- command → executor handoff → `COMMAND_SENT`;
- device interview/configure/remove/leave;
- Automation time/state/event → command;
- System time/weather → Domain;
- Wi-Fi scan/connect/known/autostart;
- Web snapshot/delta/slow client;
- Display navigation and state updates;
- persistence policy RAM/RamFlash;
- reboot after every persistent mutation;
- storage error and corrupt record behavior;
- canonical parity: Web snapshot/delta, Zigbee-проекция и storage несут
  байт-идентичные записи (zero-marshalling).

## 10. Definition of done этапа 1

Этап 1 завершён, когда:

- Domain descriptors выражают backing и `persist_key`; `key_size`/`record_size`
  производны от canonical-констант;
- раскладки `ha_rule`, `ha_group`, `ha_group_item` перенесены в `ha-model` с
  compile-time проверками; `RULE_RECORD_LEN` в `ha-automation` удалён;
- тест `desc == canonical` и layout-паритет V2↔V3 зелёные (включая
  `ha_rule_t.enabled` на смещении 384);
- Domain владеет persistence policy; mutation выполняет `micro-db` (§3.4);
- `entity_remove` следует V2: `Result<()>`, отсутствие — `NotFound`;
- storage errors не публикуют факты;
- `DomainHandle` не отдаёт `MutexGuard` (`with_mut` — до этапа 3);
- `post` вызывает executor вне Domain-lock;
- `restore` не публикует факты и не зависит от `source`;
- все контрактные тесты зелёные;
- host-сборка и host-тесты проходят; работоспособность на P4 не требуется до
  этапа 6;
- `RecordSink`/`StoreSink` ещё существуют: их удаление из production-пути —
  этап 2, проводка Domain в `main.rs` — этап 5.

После этого начинается этап 2.
