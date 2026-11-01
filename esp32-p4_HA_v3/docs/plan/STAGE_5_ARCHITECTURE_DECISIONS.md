# Stage 5 — архитектурные решения

> Статус: **архитектурные решения утверждены; Stage 5.3 закрыт, следующий этап — 5.6 migration**. 
> Stage 5.0 (аудит) завершён. Решения по manifest, RAM-only Wi-Fi/Weather
> и миграции старого Flash зафиксированы.
> Production-код в рамках Stage 5.0 не менялся.

## 1. Цель

Подключить production Domain Store к существующему `micro_db` и удалить
app-level persistence (`store::save_domain/remove_domain/restore_domain`,
`StoreSink`, cleanup в `config.rs`).

Единая граница:

```text
Services
  -> DomainHandle / DomainTransaction
     -> Domain Store
        -> micro_db Table / Storage
           -> CFlash
```

Сервисы не должны знать о `Backing`, `persist_key`, Flash layout, compaction
или `RegionView`.

## 2. Утверждённая manifest-политика

Persistent-таблицы (`Backing::RamFlash`):

| Entity | persist_key | storage |
|---|---|---|
| Logical Device | `logical_device` | RamFlash |
| Entity | `entity` | RamFlash |
| Capabilities | `capabilities` | RamFlash |
| Group | `group` | RamFlash |
| Group Item | `group_item` | RamFlash |
| Group Automation | `group_automation` | RamFlash |
| Automation | `automation` | RamFlash |
| Location | `location` | RamFlash |
| Settings | `settings` | RamFlash |
| WiFi Known | `wifi_known` | RamFlash |

RAM-only (`Backing::Ram`):

| Entity | storage |
|---|---|
| Property State | Ram |
| Weather | Ram |
| WiFi Scan | Ram |
| WiFi Status | Ram |

Это сознательное изменение поведения для Weather/WiFi Scan/WiFi Status:
сейчас они не сохраняются штатным путём, хотя старый generic restore мог
восстановить их при наличии старых записей.

`persist_key` должен быть стабильным, уникальным и не меняться после релиза.

## 3. Flash budget

Предварительная оценка для `domain` partition (`0x80000` = 512 KiB):

- 10 persistent-таблиц — примерно 156 KiB регионов;
- с Weather/WiFi Scan/WiFi Status — примерно 180 KiB;
- плюс directory `RegionManager` (2 erase-сектора);
- `RegionManager::MAX_ENTRIES = 16` относится к пользовательским регионам,
  не к служебным записям каталога.

Нужен автоматический manifest budget test:

1. Суммарный размер регионов.
2. Directory overhead.
3. Выравнивание по `erase_size`.
4. Резерв на compaction.
5. Свободный запас на будущие таблицы.
6. `MAX_ENTRIES`.
7. Проверка, что manifest не превышает `domain` partition.

Тест должен использовать реальные канонические размеры и реальную формулу
`RegionManager`, а не дублировать магические числа в тесте.

## 4. Формат старого Flash: утверждена миграция

Старый и новый layout несовместимы:

```text
old: generic DomainKey + DomainSlot в одной таблице на весь partition
new: RegionManager directory + per-table RamFlash регионы
```

Решение: **миграция с сохранением пользовательских данных**.

Dual-read/dual-write как постоянная архитектура не допускается.

Обязательные требования миграции:

1. Старый формат доступен до успешного переноса.
2. Миграция устойчива к отключению питания на любом этапе.
3. После переноса проверяется целостность всех persistent-таблиц.
4. Старый формат не стирается до подтверждения успешного восстановления нового.
5. Повторный запуск после прерванной миграции не повреждает данные и не создаёт дубликаты.
6. Переносятся только записи, предусмотренные утверждённым manifest.
7. Weather/WiFi Scan/WiFi Status из старого формата не переносятся.

Протокол power-fail-safe миграции проектируется на Stage 5.6. До этого
нельзя стирать старую таблицу.

Отдельно проверить, возможно ли временное сосуществование старого и нового
формата в пределах 512 KiB `domain` partition. Если места недостаточно,
нужен другой механизм сохранения исходных данных.

## 5. Compaction и maintenance

Compaction не должен выполняться внутри каждой мутации и не должен удерживать
весь Domain mutex на время обслуживания Flash.

Требуется отдельный maintenance gate на уровне таблицы/storage backend:

- обычная мутация: Domain mutex + короткая запись;
- maintenance: gate конкретной таблицы, без Domain mutex;
- compaction не меняет Table index/free-list;
- maintenance не получает доступа к Domain-методам сервисов.

Текущий `Table<StorageBackend>` не позволяет это безопасно: `StorageBackend`
type-erased и не предоставляет `compact`/`bank_state`; `Domain::restore`
освобождает provider. Нужен отдельный lifecycle-контракт.

Минимальные требования:

1. `compact`/`bank_state`/`sync` доступны только composition root или
   Domain maintenance API.
2. Мутации и compaction сериализуются per-table gate.
3. `NoSpace` имеет явную политику: maintenance или ошибка вызывающему.
4. Maintenance выполняется явно через `DomainMaintenance` по решению
   composition root. Автоматический startup или periodic compaction в рамках
   Stage 5 не вводится. Если перед запуском требуется обслуживание таблицы,
   composition root должен выполнить его явно до передачи Domain сервисам.
5. Долгая compaction не блокирует весь Domain.

## 6. Restore и bootstrap Display/Web

`Domain::restore()` не публикует Domain Delta. Это правильно.

Порядок запуска:

1. Создать Domain с production StorageProvider.
2. Зарегистрировать manifest.
3. `Domain::restore()`.
4. Создать `DomainHandle`.
5. Display/Web получают snapshot восстановленного состояния.
6. Только после этого запускается обычная доставка Delta.

Недостаточно простой перестановки строк: нужен согласованный
**snapshot + subscription** механизм. Иначе возможны:

- потеря изменений между snapshot и подпиской;
- получение фиктивных `EntityUpserted` при restore;
- неполный UI/Web state.

Нужен API, который под одной Domain-критической секцией:

- фиксирует snapshot/sequence;
- устанавливает подписку;
- после освобождения lock отдаёт snapshot потребителю.

Последующие Delta применяются поверх snapshot. `Domain::restore` не должен
публиковать факты.

Контракт snapshot + subscription:

```text
lock Domain
    capture canonical snapshot
    register subscriber
unlock Domain
return snapshot + subscription
```

Гарантии:

- изменения до захвата snapshot входят в snapshot;
- изменения после регистрации subscriber приходят через Delta;
- окна потери изменения между snapshot и подпиской нет.

Переполнение subscriber inbox — обнаруживаемый разрыв, а не незаметно
устаревшее состояние. При переполнении consumer обязан запросить новый
snapshot; обычная Delta-доставка после разрыва не считается достаточной.
Для Display и Web нужен явный resync path.

## 7. Удаление legacy persistence

После 5.1–5.4 удалить:

- `store::save_domain`;
- `store::remove_domain`;
- `store::restore_domain`;
- `StoreSink`/`RecordSink`;
- persistence cleanup из `config.rs`;
- отдельный `wifi::save_domain`;
- `domain_tables` с `Backing::Ram` для persistent-таблиц.

`store.rs` сохраняет только registry (`mstore`), пока это отдельная
ответственность.

## 8. Последовательность Stage 5

| Этап | Работа |
|---|---|
| 5.0 | Аудит — завершён |
| 5.1 | Production StorageProvider — завершён |
| 5.2 | Manifest + budget/validity tests — реализовано |
| 5.3 | Maintenance API, compaction, NoSpace — завершён |
| 5.6 | Power-fail-safe миграция старого Flash — следующий этап |
| 5.4 | Production wiring + restore + snapshot/subscription + UI/Web bootstrap |
| 5.5 | Удаление legacy persistence |
| 5.7 | Полные integration tests |

Production activation (5.4) запрещена до успешного завершения 5.6.
Stage 5.6 является обязательным migration gate. Snapshot/subscription и
inbox overflow policy входят в 5.4, а не в migration.
