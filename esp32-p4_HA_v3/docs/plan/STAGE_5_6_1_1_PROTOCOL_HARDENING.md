# Stage 5.6.1.1 — Migration Protocol Hardening

> Статус: **design hardening**. Мигратор не реализован.
> Исходный `STAGE_5_6_MIGRATION_PROTOCOL.md` не изменяется; этот документ
> уточняет его критические места.

## 1. Flash Geometry Audit

### Старый layout

`ha-app::store::DomainTable` использует:

```text
partition size: 0x80000 (512 KiB)
bank size:      0x40000 (256 KiB)
bank 0:         0x00000..0x40000
bank 1:         0x40000..0x80000
capacity:       384
key_size:       21
payload_size:   458
record size:    16 + 21 + 458 + 8 = 503 bytes
header:         64 bytes
```

Максимальный конец старого банка 1:

```text
0x40000 + 64 + 384 * 503 = 0x6F2C0
```

Последняя используемая область старого банка 1 заканчивается до
`0x7E000`. Marker-сектора находятся в диапазоне `0x7E000..0x80000`, внутри
физического банка 1, но за пределами максимально возможных записей старой
таблицы.

### Что делает старый FlashTable

- `open` читает header в начале банка (`0x40000`), не в конце.
- `recover` сканирует записи от `HEADER_SIZE` до первого полностью erased
  record prefix. Максимальная запись заканчивается на `0x6F2C0`, затем идёт
  erased gap, поэтому marker не участвует в recovery.
- `compact()` стирает весь целевой банк (`0x40000`, длина `0x40000`), включая
  marker-сектора.
- marker нельзя записывать до последнего compaction старого банка.
- после появления marker старый `compact()`/`clear()` запускать нельзя: он
  уничтожит marker.
- cleanup старого банка не должен использовать `FlashTable::compact()`.
  Cleanup обязан стирать только старые data-сектора и сохранять
  `0x7E000..0x80000`, либо marker должен быть перенесён в другую область до
  очистки.

### Вывод

Размещение допустимо только при следующем инварианте:

```text
Все compaction/clear старого layout выполняются до первой записи marker.
После первой записи marker старый layout только читается.
```

Marker не должен использоваться до завершения compaction старого bank1.
Новый layout использует только нижние 256 KiB; marker и old bank1 не входят в
его capacity.

## 2. Marker Specification

### Расположение

```text
slot A: 0x7E000..0x7F000
slot B: 0x7F000..0x80000
```

Оба слота — erase-сектора по 4096 байт. Запись 32-байтного header в начале
сектора; остальная часть сектора остаётся erased.

### Header

Little-endian, 32 bytes:

| Offset | Field | Size |
|---:|---|---:|
| 0 | `magic` = `0x4D494752` (`MIGR`) | u32 |
| 4 | `version` = 1 | u16 |
| 6 | `header_len` = 32 | u16 |
| 8 | `generation` | u32 |
| 12 | `state` | u8 |
| 13 | reserved = 0 | 3 bytes |
| 16 | `crc32` | u32 |
| 20 | `commit` | u32 |
| 24 | `commit_inv` | u32 |
| 28 | reserved = 0 | u32 |

`state`:

```text
1 = OLD
2 = MIGRATING
3 = NEW_VERIFIED
4 = COMMITTED
```

`crc32` — CRC-32 IEEE (reflected, polynomial `0xEDB88320`) по bytes `0..16`.

`commit`/`commit_inv`:

- для `COMMITTED`: `commit = 0x434D4954` (`CMIT`), `commit_inv = !commit`;
- для остальных состояний: `commit = 0`, `commit_inv = 0xFFFFFFFF`.

`crc32` не покрывает `commit`/`commit_inv`: они являются отдельным
commit-witness pair. Валидность `COMMITTED` требует одновременно:

```text
valid magic/version/header_len/state/crc
AND commit == 0x434D4954
AND commit_inv == !commit
AND full 32-byte read-back matches
```

### Запись marker

1. Прочитать оба слота.
2. Выбрать неактивный слот.
3. Стереть неактивный слот.
4. Проверить read-back: слот полностью erased.
5. Записать header bytes `0..20` (CRC included).
6. Выполнить read-back header `0..20`.
7. Записать `commit`/`commit_inv` последними.
8. Выполнить полный read-back 32 bytes из Flash и провалидировать.
9. Только после успешного read-back считать generation активной.

Если любой шаг сорван, предыдущий валидный marker остаётся источником истины.
Частично записанный слот игнорируется.

### Выбор слота

- Оба слота erased → `MarkerAbsent`.
- Ровно один валидный → использовать его.
- Оба валидны, generation различается → выбрать больший generation.
- Оба валидны, generation одинаковый, state/CRC/commit совпадают → duplicate;
  выбрать любую копию.
- Оба валидны, generation одинаковый, state/commit различаются →
  fail-closed, `MarkerConflict`.
- Один или оба слота non-erased и invalid → `MarkerCorrupt`; fail-closed.
- `generation == u32::MAX` → новые marker-записи запрещены; fail-closed.

## 3. Old Source Validation

Перед staging и после old compaction:

1. Открыть старый layout повторно из Flash.
2. Прочитать все записи старого generic table.
3. Сравнить количество и bytes записей с pre-compaction snapshot.
4. Убедиться, что active bank старого layout — bank1.
5. Убедиться, что marker отсутствует или явно `OLD`.

Только после этого разрешена запись `MIGRATING`.

## 4. Данные и исключения

Известные excluded types:

```text
ENTITY_PROPERTY_STATE
ENTITY_WEATHER
ENTITY_WIFI_SCAN
ENTITY_WIFI_STATUS
```

Они не переносятся по утверждённой storage policy.

Переносятся только 10 persistent-таблиц manifest.

Классификация старых записей:

- известный persistent type + корректный key/record → переносить;
- известный excluded type → намеренно не переносить;
- неизвестный entity type → ошибка миграции;
- неверный key/record size → ошибка миграции;
- повреждённый record → ошибка миграции;
- неизвестный формат → ошибка миграции.

Автоматический skip неизвестных записей и autoformat запрещены.

## 5. Recovery State Machine

Входные данные:

```text
marker = Absent | Valid(state, generation) | Corrupt | Conflict
old    = Valid | Invalid
new    = Valid | Invalid
```

Нормативная матрица:

| Marker | Old | New | Действие |
|---|---|---|---|
| Absent | Valid | Invalid | Использовать old |
| Absent | Valid | Valid | Ambiguous, fail-closed |
| Absent | Invalid | Valid | Ambiguous, fail-closed |
| Absent | Invalid | Invalid | `Corrupt`, без форматирования |
| Corrupt | Valid | Invalid | Fail-closed |
| Corrupt | Valid | Valid | Fail-closed |
| Corrupt | Invalid | Valid | Fail-closed |
| Corrupt | Invalid | Invalid | `Corrupt`, без форматирования |
| `MIGRATING` | Valid | Любое | Old; staging удалить и повторить |
| `MIGRATING` | Invalid | Любое | Fail-closed; старый источник утрачен |
| `NEW_VERIFIED` | Valid | Valid | Повторить commit |
| `NEW_VERIFIED` | Valid | Invalid | Old; staging пересоздать |
| `NEW_VERIFIED` | Invalid | Valid | Fail-closed; commit без old source |
| `COMMITTED` | Любое | Valid | New |
| `COMMITTED` | Valid | Invalid | Fail-closed, explicit recovery |
| `COMMITTED` | Invalid | Invalid | `Corrupt`, без форматирования |

Отдельные правила:

- `MarkerAbsent` не означает `OLD`; при двух валидных layout это ambiguity.
- Одинаковый generation с разными state — `MarkerConflict`.
- `COMMITTED` с невалидным new layout не откатывается автоматически на old:
  old может быть устаревшим после production-записей.
- после `COMMITTED` cleanup old не входит в recovery path.

## 6. Expanded Crash Matrix

### Old source compaction

| Сбой | Результат |
|---|---|
| erase target bank | old active bank остаётся валидным |
| copy records | old active bank остаётся валидным |
| write target header torn | target invalid; old active bank valid |
| read-back fail | migration abort; old valid |

### Marker erase

| Сбой | Результат |
|---|---|
| erase inactive marker sector | previous valid marker сохраняется |
| read-back не erased | fail-closed; previous marker |

### Marker body write

| Сбой | Результат |
|---|---|
| partial bytes | slot invalid; previous marker |
| CRC write torn | slot invalid; previous marker |
| read-back mismatch | slot invalid; previous marker |

### Commit witness write

| Сбой | Результат |
|---|---|
| partial commit | slot invalid; previous marker |
| commit_inv mismatch | slot invalid; previous marker |
| read-back mismatch | slot invalid; previous marker |
| full read-back valid | `COMMITTED` принят |

### Staging erase / write

| Сбой | Результат |
|---|---|
| erase отдельного sector | old valid; staging dirty, пересоздать |
| region directory write | old valid; staging invalid, пересоздать |
| table write | old valid; staging invalid, пересоздать |
| verify read fail | old valid; staging invalid, пересоздать |
| verify complete, before NEW_VERIFIED | old valid; повторная проверка/commit preparation |

### Commit

| Сбой | Результат |
|---|---|
| before NEW_VERIFIED | old valid |
| NEW_VERIFIED write torn | old valid |
| after NEW_VERIFIED, before COMMITTED | old valid; commit можно повторить |
| COMMITTED write torn | previous marker; old valid |
| COMMITTED read-back invalid | old valid; повторная попытка |
| COMMITTED read-back valid | new authoritative |

## 7. New Layout Validation

До `NEW_VERIFIED` и повторно после read-back:

- открыть `RegionManager` lower 256 KiB из Flash;
- открыть все persistent regions;
- сравнить count/key/record с old source;
- проверить отсутствие неизвестных/excluded записей в new;
- проверить geometry/capacity;
- проверить, что lower regions + directory <= 256 KiB;
- проверить, что marker sectors не входят в RegionManager capacity.

Сравнение выполняется с фактически прочитанным old source, а не с RAM-копией.

## 8. Budget

```text
new regions:       ~155648 bytes
new directory:       8192 bytes
total new:         ~163840 bytes
lower capacity:    262144 bytes
headroom:           ~98304 bytes
```

Old bank1 data maximum:

```text
start: 0x40000
end:   0x6F2C0
marker start: 0x7E000
gap:   0xED40 (60736 bytes)
```

Условия:

- `MAX_ENTRIES` = 16; persistent tables = 10.
- marker sectors не входят в RegionManager capacity.
- new layout capacity = 0x40000, пока old bank1 сохраняется.
- future expansion beyond lower 256 KiB запрещён до cleanup old.

## 9. Cleanup

Cleanup не входит в Stage 5.6.1.1.

Если cleanup старого bank1 будет реализован позже:

- нельзя использовать `FlashTable::compact()`, который стирает marker;
- стирать только старые data-сектора;
- сохранять `0x7E000..0x80000`;
- либо сначала перенести marker в отдельную reserved/NVS область.

## 10. Обязательные fault-injection тесты

- сбой на каждом erase/write/read-back шаге marker;
- torn commit witness;
- duplicate generation;
- generation overflow;
- marker absent vs corrupt;
- old/new ambiguity;
- old compaction failure;
- staging failure на каждом sector/table;
- verify failure;
- COMMITTED + corrupt new;
- cleanup, не затрагивающая marker;
- повторный запуск после каждого state.

## 11. Статус

Этот документ закрывает четыре вопроса Stage 5.6.1.1:

1. Flash geometry audit.
2. Marker specification.
3. Recovery state machine.
4. Expanded crash matrix.

Реализация мигратора начинается только после принятия этого документа.
