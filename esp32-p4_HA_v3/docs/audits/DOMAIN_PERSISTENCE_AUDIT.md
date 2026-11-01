# Аудит Domain persistence и restore

> Статус: **технический аудит первого слоя; Stage 1 реализован в рабочем
> дереве**. Разделы 3–4 описывают состояние до реализации и сохранены как
> снимок. Слой обычный: Zigbee-исключение к нему не применяется.
>
> План: [V2_TO_RUST_PORTING_PLAN.md](../plan/V2_TO_RUST_PORTING_PLAN.md),
> этап 1. Принципы: [V2_TO_RUST_PORTING_PRINCIPLES.md](../plan/V2_TO_RUST_PORTING_PRINCIPLES.md).

## 1. Scope

Проверить V2-источник, текущее Rust-состояние и зависимости для:

- регистрации Domain-таблиц;
- production persistence;
- restore после reboot;
- удаления параллельного app-level persistence.

Не входит в аудит: миграция старых данных, recovery state machine, изменение
Zigbee-слоя, новое snapshot-механизма, реализация кода.

## 2. V2-источник

### 2.1. Таблицы и policy

V2 `main/app_main.c` — единственное место, где заданы capacities/backing/
persist keys. Фактическая policy:

| Entity | Capacity | Backing | persist_key |
|---|---:|---|---|
| `AUTOMATION` | 32 | RAM+FLASH | `automation_v5` |
| `LOCATION` | 4 | RAM+FLASH | `location` |
| `GROUP` | 16 | RAM+FLASH | `group` |
| `GROUP_ITEM` | 128 | RAM+FLASH | `group_item_v3` |
| `GROUP_AUTOMATION` | 32 | RAM+FLASH | `group_auto_v1` |
| `SETTINGS` | 1 | RAM+FLASH | `settings` |
| `WIFI_KNOWN` | 8 | RAM+FLASH | `wifi_known` |
| `ENTITY` | 32 | RAM | — |
| `LOGICAL_DEVICE` | 32 | RAM | — |
| `CAPABILITIES` | 32 | RAM | — |
| `PROPERTY_STATE` | 256 | RAM | — |
| `WEATHER` | 1 | RAM | — |
| `WIFI_SCAN` | 1 | RAM | — |
| `WIFI_STATUS` | 1 | RAM | — |

Итого: **7 persistent** и **7 RAM-only** таблиц.

Источник:
`esp32-p4_HA_v2/main/app_main.c:38-222`.

### 2.2. Где хранится

V2 использует раздел `mstore` (`0x60000`) для Domain-таблиц. Раздел `zb_model`
хранит Zigbee-private данные. Это видно в
`esp32-p4_HA_v2/partitions.csv` и `app_main.c`.

Persistent-таблица открывается как регион mstore через
`mstore_table_init`; отдельного app-level restore нет.

### 2.3. Restore semantics

- `domain_init` создаёт Domain.
- `domain_register_entity` регистрирует таблицу; `mstore_table_init` читает
  persistent backend и восстанавливает RAM-состояние.
- Регистрация/restore **не публикует Domain facts**.
- `domain_entity_put/get/remove` работают уже с восстановленной таблицей.
- Ошибка `mstore_table_init` возвращается из `domain_register_entity`;
  bootstrap `start_step` завершает запуск с ошибкой.

Источники:
`esp32-p4_HA_v2/domain/src/domain_registry.c:151-194`,
`esp32-p4_HA_v2/domain/src/domain_entity.c:74-117,217-233`,
`esp32-p4_HA_v2/main/app_main.c:333-383`.

### 2.4. Persist keys — часть контракта

V2 использует versioned keys:

- `automation_v5` — несовместимый старый automation layout;
- `group_item_v3` — tile = entity, старый `(entity, property)` не читается;
- `group_auto_v1` — отдельная раскладка group automation.

Смена `persist_key` означает новый регион, а не миграцию старого. Это
подтверждает правило: старые данные не читаются автоматически.

## 3. Текущее Rust-состояние

### 3.1. Domain storage не подключён

`crates/ha-app/src/main.rs:60` создаёт `Domain::new_with_clock`, то есть
RAM-only Domain. `Domain::new_with_storage` и `Domain::restore` не вызываются.

`crates/ha-app/src/domain_tables.rs:139-157` делает `legacy_desc`: каждой
таблице принудительно ставит `Backing::Ram` и `persist_key=None`. Поэтому
production manifest сейчас не используется.

`crates/ha-app/src/store.rs:98-135` содержит `DomainStorageProvider`, но он
`#[allow(dead_code)]`; `store.rs:193-245` содержит отдельную generic
`DomainTable` и `DOMAIN_DB`.

### 3.2. Параллельный persistence path

`crates/ha-app/src/store.rs:398-531` реализует:

- `save_domain(entity, key, record)`;
- `remove_domain`;
- `restore_domain(domain)`.

`restore_domain` читает одну generic-таблицу
`(entity, key_len, key, record_len, record)` и вызывает `domain.entity_put`
для всех записей, кроме `PROPERTY_STATE`.

Это второй storage path и второй источник истины. Он также публикует
`ENTITY_UPSERTED` facts во время restore, чего V2 не делает.

### 3.3. Call sites обходят Domain ownership

`config::put_record` и `config::remove_record`
(`crates/ha-app/src/config.rs:14-44`) сначала меняют Domain, затем вызывают
`store::save_domain`/`remove_domain`. `wifi.rs:197` делает то же для
`WIFI_KNOWN`.

Последствие: ошибка после Domain-мутации оставляет RAM и flash в разных
состояниях; запись в Domain не является атомарной с persistence.

### 3.4. Manifest отличается от V2

`crates/ha-app/src/domain_tables.rs:22-136`:

- 10 таблиц помечены `RamFlash`;
- `LOGICAL_DEVICE`, `ENTITY`, `CAPABILITIES` persistent, хотя в V2 они RAM;
- ключи `automation`, `group_item`, `group_automation` не совпадают с V2
  (`automation_v5`, `group_item_v3`, `group_auto_v1`);
- остальные capacities совпадают с V2.

Текущий manifest нельзя считать V2-политикой. Это не улучшение, которое
нужно сохранить: persistent canonical Entity/Device/Capabilities дублирует
adapter-private registry и создаёт второй источник lifecycle-данных.

### 3.5. Раздел и keys

Текущий Rust использует:

```text
mstore 0x60000 — Zigbee Registry
domain 0x80000 — Domain storage
```

V2 использовал `mstore` для Domain. Это **platform adaptation текущего Rust**:
разделы разнесены, `DomainStorageProvider` уже читает `CFlash::DOMAIN`.
Контракт Domain (table schema, RegionManager/FlashTable) от этого не меняется.
Но это означает, что V2 `mstore`-данные не находятся в текущем `domain`
разделе; автоматической совместимости с V2 flash нет.

### 3.6. Restore API уже готов

`crates/ha-domain/src/domain.rs:73-145`:

- `new_with_storage` создаёт Domain в состоянии `Registering`;
- `restore` открывает backend для каждой зарегистрированной таблицы,
  создаёт `Table`, собирает `RestoreReport`, не публикует facts;
- при ошибке restore остаётся retryable;
- `take_maintenance` отдаёт maintenance handles.

`crates/ha-domain/tests/stage1c.rs` уже проверяет:

- restore preserves flash records/versions;
- restore failure atomic/retryable;
- restore does not publish facts;
- storage write error returns error without fact.

Эти тесты можно использовать как основу; их manifest — тестовый, не
production.

### 3.7. Ошибки запуска

Сейчас `main.rs:85-88` логирует ошибку `domain_tables::register` и продолжает;
`restore_domain` тоже только логирует read failure. V2 завершает bootstrap при
ошибке регистрации/инициализации persistent table.

Для Stage 1 нужно выбрать V2-поведение: ошибка persistent Domain storage
останавливает startup, а не оставляет приложение с частичным состоянием.

### 3.8. Тесты

- `ha-domain/tests/stage1c.rs` — restore/no-facts/atomicity.
- `ha-domain/tests/stage5_maintenance.rs` — maintenance; не обязателен для
  Stage 1.
- `ha-app/src/domain_tables.rs` tests проверяют текущий manifest (10
  persistent), а не V2 policy (7 persistent).
- Нет ha-app integration test для `DomainStorageProvider` + production
  manifest + reboot.

## 4. Gap list

| Приоритет | Gap | Следствие |
|---|---|---|
| P0 | `Domain::new_with_storage`/`restore` не используются | persistent config не живёт в Domain |
| P0 | `store::save_domain`/`restore_domain`/`DOMAIN_DB` | второй storage path, restore публикует facts |
| P0 | manifest: 10 persistent вместо V2 7 | неверная policy, лишние регионы, lifecycle дублируется |
| P0 | persist keys не V2 | несовместимость с V2 keys/раскладками |
| P0 | config/wifi вызывают `save_domain` | RAM/flash divergence |
| P1 | startup продолжается после storage error | работа с частичным состоянием |
| P1 | старый `domain` partition может быть в generic layout | `RegionManager` не должен автоформатировать |
| P1 | нет production integration test | нельзя доказать reboot/restore |
| P2 | `DomainMaintenance` не подключён | не блокер; не добавлять startup/periodic compaction |
| P2 | `store.rs` смешивает registry и Domain | путаница владения |

## 5. Решение по scope Stage 1

Stage 1 должен только завершить **V2 Domain persistence**, не создавая
migration/recovery механизмов.

### 5.1. Целевая policy

- 7 persistent таблиц с V2 `persist_key` и RAM+FLASH backing;
- 7 RAM-only таблиц;
- `PROPERTY_STATE` всегда RAM;
- capacities — из V2 `app_main.c`;
- текущий раздел `domain` 512 KiB — platform adaptation, сохраняется;
- `mstore` остаётся разделом Zigbee Registry.

### 5.2. Целевой startup

```text
create Domain with DomainStorageProvider
register V2 descriptors
restore Domain
subscribe/start services
```

`Domain::restore` до запуска сервисов. Restore не публикует facts.

### 5.3. Что удалить после замены

- `store::save_domain`, `remove_domain`, `restore_domain`;
- `DOMAIN_DB` и generic `DomainTable`;
- `domain_tables::legacy_desc`;
- вызовы `save_domain`/`remove_domain` из `config.rs` и `wifi.rs`;
- `Domain::new_with_clock` в production bootstrap (заменить на
  `new_with_storage`).

`store.rs` сохраняет только Zigbee Registry (`mstore`).

### 5.4. Чего не делать

- не добавлять migration markers или recovery state machine;
- не добавлять startup/periodic compaction;
- не делать autoformat при несовместимом старом `domain` partition;
- не сохранять persistent Entity/Capabilities/Logical Device только ради
  удобства;
- не менять Zigbee-слой;
- не добавлять snapshot/subscription механизмы.

## 6. Data precondition

**Решение оператора: содержимое `domain` можно потерять.**
Перенос старых записей не выполняется, но сброс остаётся отдельной явной
операцией.

Read-only процедура проверки:
[DOMAIN_PARTITION_DIAGNOSTIC.md](DOMAIN_PARTITION_DIAGNOSTIC.md).
Порядок явного сброса:
[DOMAIN_RESET_PROCEDURE.md](DOMAIN_RESET_PROCEDURE.md).

Текущий `domain` partition может содержать generic-таблицу старого Rust
layout. `RegionManager` ожидает RegionManager directory; старый формат не
является валидным directory. При несовпадении приложение должно завершиться
fail-closed, а не форматировать раздел автоматически.

Перед Stage 1 всё равно обязательны:

1. read-only dump и SHA256;
2. фиксация решения «данные можно потерять»;
3. отдельный erase только `domain`;
4. прошивка Stage 1 в том же окне, без загрузки старого firmware после erase.

## 7. Тестовый план Stage 1

1. V2 manifest test: 7 persistent + 7 RAM; keys/backing/capacities.
2. Region budget test для 7 persistent таблиц.
3. `Domain::new_with_storage` + production manifest + `restore` на SimFlash.
4. Reboot: put → drop → reopen → restore → read; version preserved.
5. Restore не публикует facts.
6. Ошибка открытия одной таблицы → restore error, startup fail-closed.
7. Запись persistent table идёт через Domain без `save_domain`.
8. `PROPERTY_STATE`/Weather/WifiScan/WifiStatus после reboot пусты.
9. Удаление/rename persistent records переживает reboot.
10. Старый generic `domain` layout → `RegionManager` fail-closed, без erase.

## 8. Acceptance criteria

- production bootstrap использует `DomainStorageProvider` и
  `Domain::restore`;
- `domain_tables` соответствует V2 policy;
- `store::save_domain`/`remove_domain`/`restore_domain`/`DOMAIN_DB` удалены;
- `config`/`wifi` не пишут второй storage path;
- restore не публикует facts;
- startup не продолжается после persistent storage error;
- тесты 1–10 зелёные;
- Zigbee слой не изменён;
- migration/recovery кода нет.

## 9. Решения и остаток

Закрыто:

1. Содержимое `domain` можно потерять; migration не выполняется. Dump и
   SHA256 всё равно обязательны до reset.
2. Сброс `domain` — отдельная явная операция; `RegionManager` не форматирует
   раздел автоматически.
3. `DomainMaintenance` остаётся API, но не подключается в Stage 1.

Остаётся решить перед кодом:

1. Проверять `CFlash::DOMAIN` capacity в runtime или полагаться на manifest
   budget test? Поведение V2 — fail startup при ошибке storage.
