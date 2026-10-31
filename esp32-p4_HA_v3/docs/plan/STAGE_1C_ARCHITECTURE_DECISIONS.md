# Stage 1c — архитектурные решения

> Статус: **предложения для согласования**. Код не менялся, Stage 1c не начат.
>
> Источники истины: V2 `docs/domain/*`, V2 `main/app_main.c`, V3
> `docs/plan/V3_IMPLEMENTATION_PLAN.md`, фактический код `ha-domain`, `micro-db`,
> `ha-app`, `zigbee-devices` и ABI Stage 1b в `ha-model`.

## 1. ERROR-факты при storage-ошибках

### 1.1. Текущий контракт

- `Domain::entity_put()` проверяет размеры и `source`, затем вызывает
  `put_record()`. Любая ошибка `put_record`, включая ошибку Storage,
  превращается в `FactKind::Error` через `publish_error()`
  (`crates/ha-domain/src/lib.rs:207-243,474-494,548-567`).
- `Domain::entity_remove()` делает то же для любой ошибки, включая
  `NotFound` (`lib.rs:267-289`).
- `Table::allocate/update/free` меняют derived state (index/free/live) только
  после успешного `Storage::write_*`
  (`crates/micro-db/src/table.rs:229-267,269-293,295-308`).
- `RamFlashStorage` пишет Flash первым, затем RAM
  (`crates/micro-db/src/storage.rs:385-393`).
- V2 `entity_op_error()` публикует `ERROR`-факт при ошибках storage
  (`esp32-p4_HA_v2/domain/src/domain_entity.c:64-72`).
- V2 `remove_locked()` при `NOT_FOUND` возвращает ошибку **без** `ERROR`-факта
  (`domain_entity.c:173-201`).
- План V3 §4.5 говорит: при ошибке storage возвращается ошибка, факт не
  публикуется, RAM соответствует последнему commit
  (`docs/plan/V3_IMPLEMENTATION_PLAN.md:384-396`).

Итог: текущее V3-поведение и V2 расходятся с формулировкой плана; а
`entity_remove(NotFound)` в V3 имеет отдельный дефект.

### 1.2. Варианты

**A. Не публиковать Domain Fact при storage-ошибке.**

- `entity_put`/`entity_remove` возвращают `Err`.
- `EntityUpserted`/`EntityRemoved` не публикуются.
- `FactKind::Error` также не публикуется.
- `entity_remove(NotFound)` — отдельный дефект: ошибка без факта.

**B. Публиковать `ERROR`-факт, но не state-факт.**

- `ERROR` считается event-like диагностикой, а не изменением state.
- Сохраняет V2-поведение и даёт подписчикам асинхронный сигнал.
- Требует, чтобы `ERROR` не путали с canonical state.

**C. Публиковать `ERROR` только для не-storage ошибок.**

- Смешивает две классификации и создаёт неочевидный контракт.
- Не рекомендуется.

### 1.3. Оценка

| Критерий | A | B |
|---|---|---|
| Соответствие плану §4.5 | да | нет |
| Соответствие V2 | нет | да |
| Canonical state не меняется | да | да |
| Диагностика через Journal | теряется; остаётся `Err` | сохраняется |
| Риск «ложного факта» | минимальный | зависит от трактовки `ERROR` |
| Сложность | низкая | низкая |

Ошибка Storage — это результат выполнения операции, а не изменение
канонического состояния. `ERROR`-факт не является state-фактом, но план
прямо запрещает факт при storage-ошибке. Кроме того, вызывающая сторона уже
получает точный `Error`; сервис может залогировать его сам.

Частичный физический I/O:

- `Table` не меняет index/free/live до успешного `write_*`.
- `RamFlashStorage` при ошибке Flash не меняет RAM.
- Ошибка после успешного Flash-commit, но до RAM-write теоретически оставила
  бы Flash впереди RAM; для `RamStorage` это практически исключено
  pre-check'ами, но контракт должен считать состояние «последний успешный
  commit», а не «частично записанные байты».
- Torn-запись на Flash восстанавливается штатным recovery; Domain не должен
  публиковать state-факт по факту частичной записи.

### 1.4. Рекомендация

**Вариант A.** При любой ошибке Storage:

- вернуть `Err`;
- не публиковать `EntityUpserted`, `EntityRemoved` и `ERROR`;
- не менять RAM/index/free/live;
- считать canonical state последним успешным commit.

`entity_remove(NotFound)` исправить отдельно: `NotFound` без факта, как в V2.
Это отдельный дефект, не часть Stage 1c.

**Последствие:** V3 осознанно расходится с V2 по `ERROR`-фактам; это нужно
утвердить. Если нужна асинхронная диагностика, её следует добавлять явным
механизмом, а не возвращать `ERROR` в Journal.

**Что утвердить:** «Storage error → только `Err`, без Domain Fact» и
«`entity_remove(NotFound)` — отдельный дефект».

## 2. RestoreReport и RestoreError

### 2.1. Текущий контракт

`Domain::restore()` отсутствует. Сейчас:

- `register_entity()` сразу открывает `RamStorage`
  (`ha-domain/src/lib.rs:179-205`);
- `EntityTable` содержит обязательную `Table<RamStorage>`
  (`lib.rs:82-85`);
- `store::restore_domain()` — app-level обход одной таблицы через
  `entity_put` с `SOURCE_ZIGBEE` и `web::upsert`
  (`ha-app/src/store.rs:440-487`);
- отдельного `RestoreReport`/`RestoreError` нет.

### 2.2. Варианты

**A. Fail-fast, атомарная установка таблиц.**

1. Открыть storage и `Table` для всех descriptors во временный вектор.
2. При первой ошибке дропнуть временные таблицы и вернуть
   `RestoreError { entity, phase, error }`.
3. Только после успеха всех таблиц установить их в `Domain`.
4. Provider не уничтожать при ошибке; разрешить повторный `restore`.

**B. Per-entity results.**

1. Попытаться открыть все таблицы.
2. Вернуть результат по каждой Entity.
3. Частично восстановленный Domain либо разрешён, либо нет.

### 2.3. Оценка

| Критерий | A | B |
|---|---|---|
| Domain atomicity | полная | требует частичного Domain |
| Поведение сервисов | Domain либо готов, либо нет | нужен контракт «частично готов» |
| Повторный restore | простой, provider сохранён | сложнее при частичной установке |
| Диагностика | одна ошибка с entity | полная карта ошибок |
| Сложность | ниже | выше |
| Риск неполного состояния | минимальный | высокий |

Per-entity report полезен для диагностики, но ценой частично доступного
Entity Store. В V2 регистрация таблицы либо открывала её, либо возвращала
ошибку; частичного Domain не было. Сервисы не должны стартовать на
неполном наборе таблиц.

### 2.4. Рекомендация

**Вариант A.** `restore()` возвращает:

```text
Result<RestoreReport, RestoreError>

RestoreReport { entities: Vec<EntityRestore> }
EntityRestore { entity, records }

RestoreError { entity, phase, error }
RestorePhase = StorageOpen | TableOpen
```

`records` — `Table::len()` после открытия. `error` — исходный
`micro_db::Error` без перекодирования. При ошибке временные таблицы не
устанавливаются; provider остаётся доступным; повторный `restore` разрешён.

**Что утвердить:** fail-fast, атомарная установка, `RestoreError` с
`entity` + `phase`, report только при успехе.

## 3. Обнаружение неизвестных регионов

### 3.1. Фактическое ограничение

`RegionManager` публично предоставляет только:

- `new()`;
- `lookup(persist_key)`;
- `bind(persist_key, capacity, key_size, payload_size)`.

См. `crates/micro-db/src/region.rs:69-193`. Перечисления directory нет.
Directory хранит 16-байтовый `persist_id` (FNV-хеш), а не исходный
`persist_key`.

Следствия:

- нельзя узнать, какие ещё регионы есть во Flash;
- нельзя сопоставить неизвестный 16-байтовый id с semantic `persist_key`;
- нельзя отличить новый `persist_key` от старого региона с несовместимой
  схемой;
- `bind()` при отсутствии записи создаёт новый регион и фиксирует directory.

### 3.2. Варианты

**A. Отложить enumeration.**

- Stage 1c проверяет только descriptors из manifest.
- Неизвестные регионы не обнаруживаются.
- Ограничение явно документируется.
- Enumeration — отдельный этап/изменение Region Manager.

**B. Расширить Region Manager.**

- Добавить read-only snapshot directory: `persist_id`, offset, size.
- Сравнить набор id с ожидаемыми хешами manifest.
- Обнаруживать лишние id; отсутствие ожидаемого региона после bind — не
  ошибка (fresh), но несовпадение размера — ошибка.
- Изменяет публичный контракт Stage 1a и требует новых тестов.

### 3.3. Оценка

| Критерий | A | B |
|---|---|---|
| Scope Stage 1c | не расширяется | расширяется |
| Обнаружение лишних регионов | нет | частично |
| Идентификация старого ключа/версии | нет | только по hash, без имени |
| Риск silent skip | явно ограничен | снижен, но не устранён полностью |
| Влияние на Stage 1a | нет | новый API и тесты |

Даже вариант B не решает полностью вопрос версии: directory не хранит
строку `persist_key` и schema version. Он может обнаружить «лишний id», но
не объяснить, какой это старый ключ. Для версии нужны либо retired-key
manifest, либо metadata с версией — это задача Stage 5.

### 3.4. Рекомендация

**Вариант A.** В Stage 1c:

- restore открывает только зарегистрированные descriptors;
- неизвестные регионы не обнаруживаются;
- это фиксируется как известное ограничение, а не как гарантия;
- enumeration, orphan detection и schema/version metadata — отдельный этап
  (кандидат — Stage 5 вместе с миграцией/erase).

**Что утвердить:** Stage 1c не обещает обнаружение неизвестных регионов;
Region Manager не меняется; ограничение считается принятым.

## 4. Регистрация Entity после Restore

### 4.1. Текущий контракт

Сейчас `register_entity()` сразу открывает таблицу, потому что
`Domain::new` — RAM-only, а storage-provider отсутствует
(`ha-domain/src/lib.rs:179-205`). Фаз жизненного цикла нет.

### 4.2. Варианты

**A. Запретить `register_entity` после успешного `restore`.**

- До restore: descriptors накапливаются, таблицы не открыты.
- Restore: открывает все таблицы атомарно.
- После restore: набор Entity фиксирован; регистрация → `InvalidState`.

**B. Разрешить регистрацию после restore.**

- Provider должен оставаться в Domain.
- Новая таблица открывается немедленно.
- Нужен контракт для таблицы, добавленной после старта сервисов.
- Усложняет инвариант «restore покрывает все descriptors».

### 4.3. Оценка

| Критерий | A | B |
|---|---|---|
| Предсказуемость lifecycle | высокая | ниже |
| Атомарность restore | сохраняется | сохраняется только для стартового набора |
| Динамическое добавление Entity | нет | есть |
| Владение provider | можно освободить после restore | provider живёт всё время |
| Синхронизация/тесты | проще | сложнее |

Динамическое добавление Entity сейчас не требуется: все canonical entities
известны bootstrap. V2 регистрировала фиксированный набор descriptors.
Разрешение после restore создаёт вопрос, что делать с уже работающими
подписчиками и отчётом restore.

### 4.4. Рекомендация

**Вариант A.** Контракт:

```text
Domain::new                 -> RamOnly: register открывает RAM сразу
Domain::new_with_storage    -> Registering: register только descriptor
Domain::restore             -> Restored: таблицы доступны
register после Restored     -> InvalidState
restore повторно после Ok   -> InvalidState
restore после ошибки        -> разрешён (Registering)
```

`entity_*` в `Registering` для ещё не открытой таблицы → `InvalidState`.
Provider хранится до успешного restore и освобождается после него.

**Что утвердить:** регистрация после restore запрещена; lifecycle
`Registering → Restored`; повторный restore после ошибки разрешён.

## 5. Descriptor manifest

### 5.1. Текущее состояние

- `DomainAdapter` содержит descriptors, но все `Backing::Ram` и часть
  констант семантически неверна (`ENTITY_AUTOMATION` использует
  `GROUP_KEY_LEN`, `ENTITY_GROUP_AUTOMATION` — `GROUP_ITEM_KEY_LEN`)
  (`crates/zigbee-devices/src/domain_adapter.rs:74-188`).
- Production `main.rs` пока не использует новый storage path
  (`ha-app/src/main.rs:57-86`).
- `ha-app` исключён из host workspace (`Cargo.toml`), поэтому manifest в
  `ha-app` недоступен host-тестам Stage 1c.
- V2 `main/app_main.c:70-222` — фактический источник descriptors для
  production: backing, capacity, persist_key.

### 5.2. Варианты

**A. Test-only manifest в Stage 1c.**

- Host-тесты `ha-domain` содержат таблицу descriptors.
- Размеры key/record берутся из `ha_model::canonical`.
- Backing/capacity/persist_key берутся из V2 `app_main.c`.
- Production `ha-app` не меняется.

**B. Bootstrap manifest в `ha-app`, не подключённый к main.**

- Плюс: будущий production source of truth.
- Минус: host-тесты его не видят; `ha-app` ESP-зависим; риск
  преждевременной проводки и дублирования.

**C. Manifest в `ha-domain` production API.**

- Делает Domain знающим canonical entity types, что нарушает границу слоёв.
- Не рекомендуется.

### 5.3. Оценка

| Критерий | A | B |
|---|---|---|
| Host-тестируемость | да | нет без отдельной сборки |
| Изменения `ha-app` | нет | да |
| Риск дублирования | временный, до Stage 5 | выше |
| Соответствие границам | да | да, но преждевременно |
| Подготовка Stage 5 | явный перенос | частичная |

### 5.4. Рекомендация

**Вариант A.** В Stage 1c:

- manifest только в host-тестах `ha-domain` (интеграционный тест);
- `key_size`/`record_size` — из `ha-model`, не литералы;
- `Backing`, `capacity`, `persist_key` — явные policy-поля;
- тест `desc == canonical` для размеров;
- тест уникальности `persist_key`;
- текущий `DomainAdapter` и `main.rs` не трогать.

В Stage 5 тот же manifest переносится в production bootstrap (`ha-app` или
host-visible composition-root модуль), а не копируется. До Stage 5
единственный экземпляр остаётся в тестах.

**Что утвердить:** test-only manifest в Stage 1c; production manifest —
Stage 5; `DomainAdapter` не меняется.

## 6. Дополнительные проверки

1. **Уникальность `persist_key`.** Проверять в `Domain::register_entity`
   до добавления descriptor; сравнивать `Option<&str>` среди всех
   зарегистрированных. Дубликат → `AlreadyExists` (или `InvalidArg`, если
   куратор предпочитает descriptor-ошибку). Пустой ключ → `InvalidArg`.
2. **`RamFlash` без `persist_key`.** Отклонять `InvalidArg`, как V2
   `desc_validate` (`domain_registry.c:35-48`).
3. **`Backing::Flash`.** Оставить неподдерживаемым в Stage 1c:
   `InvalidState`. Реализовывать только при реальном потребителе.
4. **Directory после неудачного restore.** `bind()` может зафиксировать
   directory для первых регионов до ошибки на следующем. Domain atomicity
   сохраняется, storage metadata atomicity — нет. Записи остаются `USED`,
   удаления региона нет; повторный restore может их переиспользовать. Это
   известное ограничение, отдельное от Domain.
5. **Atomicity.** Гарантировать только Domain atomicity; storage metadata
   atomicity в Stage 1c не обещать. Для неё нужен `prepare/commit` или
   cleanup в Region Manager — отдельное изменение.
6. **StorageOpen vs TableOpen.** В `RestoreError` различать фазу:
   `StorageOpen` — provider/`RamFlashStorage::open`; `TableOpen` —
   `Table::open`/index rebuild. Исходный `Error` не перекодировать.
7. **Повторяемость restore.** Provider не `take()`-ать до успеха; временные
   таблицы дропать при ошибке; после ошибки Domain остаётся в
   `Registering`, повторный restore разрешён. Directory-записи первых
   bind’ов могут остаться и будут переиспользованы.

## 7. Сводная таблица рекомендаций

| Вопрос | Рекомендация | Статус |
|---|---|---|
| ERROR-факты | Storage error → только `Err`, без Domain Fact | требует утверждения |
| RestoreReport | Fail-fast, атомарная установка, `RestoreError { entity, phase, error }` | требует утверждения |
| Unknown regions | Enumeration отложить; Stage 1c не обещает обнаружение | требует утверждения |
| Registration lifecycle | После успешного restore запрещена; повторный restore после ошибки разрешён | требует утверждения |
| Descriptor manifest | Test-only в Stage 1c; production — Stage 5 | требует утверждения |

## 8. Ограничения Stage 1c

- Не обнаруживает неизвестные/осиротевшие регионы.
- Не гарантирует storage metadata atomicity при неудачном restore.
- Не поддерживает `Backing::Flash`.
- Не поддерживает регистрацию Entity после restore.
- Не выполняет миграцию или erase старой раскладки `ha_domain`.
- Не меняет production `main.rs`, `store.rs`, `DomainAdapter`.
- Не меняет Region Manager.

## 9. Что нельзя включать в Stage 1c

- Enumeration/cleanup/orphan detection в Region Manager.
- Schema/version metadata и миграцию.
- Production wiring и per-entity flash partition sizing.
- Изменения `Domain::post`, `DomainHandle::lock`, `with_mut` (Stage 1d).
- Удаление `RecordSink`/`StoreSink` (Stage 2).
- Исправление `entity_remove(NotFound)` как часть Stage 1c (отдельный
  дефект).

## 10. Зависимости от Stage 5

- Production descriptor manifest и bootstrap wiring.
- Размер partition и проверка ёмкости Region Manager.
- Разовый erase/миграция старой `ha_domain`.
- Orphan/unknown-region cleanup и schema/version policy.
- Реальная проверка на P4.

## 11. Предлагаемый окончательный контракт Restore

```text
StorageBackend: Storage + Send
StorageProvider: Send
  open(&mut self, desc: &EntityDesc) -> Result<StorageBackend>

Domain::new_with_storage(journal, payload, payload_max, clock, provider)
Domain::register_entity(EntityDesc) -> Result<()> // только до restore
Domain::restore() -> Result<RestoreReport, RestoreError>

RestoreReport { entities: Vec<EntityRestore> }
EntityRestore { entity, records }
RestoreError { entity, phase, error }
RestorePhase = StorageOpen | TableOpen
```

Инварианты:

- restore не публикует Domain Facts;
- restore не использует `source`;
- при ошибке таблицы в Domain не устанавливаются;
- provider сохраняется для повторной попытки;
- после успеха provider освобождается, регистрация запрещена;
- `RamFlashStorage::open` выполняет hydrate, `Table::open` — index rebuild;
- `version` записей сохраняется.

## 12. Вопросы, требующие решения куратора

1. Принять ли отсутствие `ERROR`-факта при storage-ошибке, несмотря на
   расхождение с V2?
2. Принять ли fail-fast restore и `RestoreError { entity, phase, error }`
   вместо per-entity результатов?
3. Принять ли явное ограничение «unknown regions не обнаруживаются» и
   отложить enumeration?
4. Запретить ли регистрацию после restore и разрешить ли повторный restore
   только после ошибки?
5. Оставить ли manifest test-only в Stage 1c и перенести его в Stage 5?
6. Подтвердить ли, что `entity_remove(NotFound)` исправляется отдельно, вне
   Stage 1c.
