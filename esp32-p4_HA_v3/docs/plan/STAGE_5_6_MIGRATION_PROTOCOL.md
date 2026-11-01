# Stage 5.6.1 — Migration Protocol Design

> Статус: **design proposal**. Мигратор ещё не реализован.
> Production activation (5.4) запрещена до успешного завершения этой миграции.

## 1. Цель

Перенести старый generic `ha_domain` layout в per-table `RegionManager`
layout без потери единственной копии пользовательских данных при отключении
питания.

Миграция должна быть самостоятельной операцией, не зависящей от production
`main` и `Domain::new_with_storage`.

## 2. Целевая раскладка `domain` partition

Используется существующий раздел `domain` размером 512 KiB:

```text
0x00000..0x40000   old bank 0 / new RegionManager directory + regions
0x40000..0x7E000   old bank 1 (source copy during migration) / unused after commit
0x7E000..0x80000   migration marker, two erase sectors
```

Старый `FlashTable` использует два банка по 256 KiB:

```text
old bank0: 0x00000..0x40000
old bank1: 0x40000..0x80000
```

Старый generic table имеет capacity 384 и slot size 458, поэтому его данные
занимают меньше 256 KiB в одном банке. Marker в последних двух erase-секторах
не пересекается с максимальным объёмом старой таблицы.

Новый layout во время миграции использует только нижние 256 KiB:

```text
RegionManager directory: 2 erase sectors
regions:                  new per-entity tables
```

Предварительный budget новых persistent-таблиц — около 156 KiB плюс directory
8 KiB. Это меньше 256 KiB. Точный budget проверяется manifest-тестом.

## 3. Подготовка старого источника

Перед staging:

1. Открыть старый generic `DomainTable`.
2. Проверить валидность старого layout и geometry.
3. Убедиться, что активный банк содержит последнюю версию данных.
4. Если активен bank0 — выполнить старую `FlashTable::compact`, чтобы
   последняя копия данных оказалась в bank1.
5. Проверить bank1 после compaction.

Compaction old table выполняется только при валидном старом layout.
Ошибка compaction останавливает миграцию без изменения активного bank.

## 4. Migration marker

В последних двух erase-секторах `domain` используется ping-pong marker:

```text
struct MigrationMarker {
    magic,
    version,
    generation,
    state,
    crc,
}
```

Запись marker:

1. Стереть неактивный marker-сектор.
2. Записать body.
3. Записать header/CRC последним.
4. Только после успешной записи считать generation активной.

При повреждении одной копии используется другая валидная копия.
Если обе копии невалидны, автоматическое форматирование запрещено.

Состояния:

| State | Значение |
|---|---|
| `OLD` / отсутствует | действует старый layout |
| `MIGRATING` | old остаётся источником истины; новый layout — staging |
| `NEW_VERIFIED` | новый layout записан и проверен; old ещё сохранён |
| `COMMITTED` | действует новый layout; old сохраняется до отдельной cleanup |

## 5. Порядок миграции

```text
1. Validate old layout
2. Ensure newest old data is in old bank1
3. Write marker MIGRATING
4. Erase lower 256 KiB staging area
5. Write new per-table regions
6. Verify new layout against old source
7. Write marker NEW_VERIFIED
8. Write marker COMMITTED
9. Keep old bank1 until explicit cleanup
```

### Commit point

Commit point — успешная запись marker `COMMITTED` с валидным CRC.

До `COMMITTED` действующим считается старый layout.

После `COMMITTED` действующим считается новый layout, даже если old bank1 ещё
физически присутствует.

Marker `NEW_VERIFIED` не является commit point. Он означает только, что новый
layout готов к переключению.

## 6. Power-fail matrix

| Момент сбоя | Действующий источник |
|---|---|
| до marker / во время чтения old | old layout |
| во время old compaction | old bank, который остался валидным |
| после compaction, до marker MIGRATING | old bank1 |
| MIGRATING, запись staging | old bank1; staging можно удалить и повторить |
| после записи staging, до проверки | old bank1 |
| проверка успешна, до NEW_VERIFIED | old bank1; миграцию можно повторить или перепроверить |
| NEW_VERIFIED, до COMMITTED | old bank1 |
| сбой записи COMMITTED | либо NEW_VERIFIED, либо COMMITTED; old bank1 валиден |
| после COMMITTED | new layout |
| cleanup old bank1 | new layout; old больше не является источником |

Ни один переход не стирает единственную валидную копию пользовательских
данных.

## 7. Повторный запуск

Поведение после reboot:

- marker `OLD`/отсутствует, old валиден → работа со старым layout;
- marker `MIGRATING`, old валиден → удалить staging и начать миграцию заново;
- marker `NEW_VERIFIED`, old и new валидны → повторить commit marker;
- marker `COMMITTED`, new валиден → использовать new;
- marker `COMMITTED`, new невалиден, old валиден → fail-safe: не форматировать
  автоматически; требуется explicit recovery, так как old может быть уже
  устаревшим после production-записей;
- marker невалиден, old и new одновременно валидны → не выбирать layout
  автоматически; сообщить о неоднозначности;
- old невалиден, new невалиден → ошибка `Corrupt`; автоформат запрещён.

## 8. Данные

Из старого generic layout переносятся только записи утверждённого manifest:

Persistent:
- Logical Device
- Entity
- Capabilities
- Group
- Group Item
- Group Automation
- Automation
- Location
- Settings
- WiFi Known

Не переносятся:
- Property State;
- Weather;
- WiFi Scan;
- WiFi Status.

Неизвестная entity type, неверный размер ключа/записи, повреждённый record или
неизвестный формат — ошибка миграции. Автоматический skip/format запрещён.

Перенос выполняется побайтово по canonical key/record. Версии нового layout
начинаются с естественных версий новых таблиц; старые runtime-версии не
являются частью старого generic record и не переносятся.

## 9. Проверка нового layout

До `NEW_VERIFIED`:

- открыть все per-table regions;
- сравнить count/key/record каждого перенесённого record со старым source;
- проверить, что отсутствуют записи вне manifest;
- проверить, что отсутствуют unknown/corrupt записи;
- проверить geometry/capacity всех регионов;
- проверить, что новый layout помещается в нижние 256 KiB;
- проверить, что marker-сектора не входят в RegionManager capacity.

Только после полной проверки разрешена запись `NEW_VERIFIED`.

## 10. Cleanup

Old bank1 не удаляется автоматически во время миграции.

После `COMMITTED` и успешного production-запуска допускается отдельная
cleanup-операция. До неё old copy остаётся как аварийная копия, но не является
действующим layout.

Cleanup не входит в Stage 5.6.1.

## 11. Границы

Не входит в миграцию:

- production `main` integration;
- `Domain::new_with_storage` activation;
- snapshot/subscription Display/Web;
- удаление legacy persistence;
- автоматический startup/periodic compaction;
- изменение canonical ABI.

Миграция не должна выполняться автоматически на обычном production boot.
Она запускается отдельным migration режимом/утилитой после явного решения.

## 12. Обязательные тесты миграции

- power-fail перед marker, во время compaction, staging, verify, commit;
- повторный запуск после каждого состояния;
- идемпотентность без duplicate records;
- marker ping-pong и torn write;
- старый layout остаётся доступным до `COMMITTED`;
- новый layout проверяется до commit;
- неизвестная/corrupt запись останавливает миграцию;
- 512 KiB budget с marker reservation;
- new layout corruption после commit не приводит к autoformat;
- old/new ambiguity не разрешается молча.

## 13. Открытые решения

1. Точная граница нового staging capacity: 256 KiB или меньше с учётом marker.
2. Формат marker header/length/CRC.
3. Нужен ли отдельный migration tool/binary или build-time mode.
4. Политика explicit recovery при `COMMITTED` + corrupt new layout.
5. Срок хранения old bank1 после commit.
