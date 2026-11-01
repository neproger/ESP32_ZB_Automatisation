# Диагностика раздела `domain`

> Статус: **read-only процедура**. Не выполнять reset/erase/format.
> Цель — установить состояние `domain` до Stage 1 и сохранить дамп.
>
> Связанный аудит: [DOMAIN_PERSISTENCE_AUDIT.md](DOMAIN_PERSISTENCE_AUDIT.md).

## 1. Что проверяем

Текущий Rust-код может оставить в разделе `domain` один из вариантов:

1. **Old generic layout** — `FlashTable`/`RamFlashStorage` из `store.rs`:
   header `MSTB`, две банки по 256 KiB, records `entity + key + slot`.
2. **RegionManager layout** — directory `MSDG` и per-table regions.
3. **Пустой раздел** — все байты `0xFF`.
4. **Неизвестное/повреждённое состояние**.

Это отличается от вопроса «есть ли данные V2 в разделе `mstore`»: V2 `mstore` и
текущий Rust `domain` — разные разделы. Отсутствие V2-совместимости в
`domain` не означает отсутствие ценных данных в текущем Rust layout.

## 2. Жёсткие ограничения

Запрещено до отдельного решения:

- `erase_flash`, `idf.py erase-flash`;
- запись в `domain` (`write_flash`, `write_bin`, `format`);
- запуск production-прошивки, которая может изменить раздел;
- автоматическое форматирование при несовпадении layout.

Разрешено только:

- чтение раздела;
- сохранение полного дампа;
- анализ дампа на host.

`RegionManager` при несовпадении layout должен завершиться fail-closed, а не
форматировать раздел. Диагностика не меняет это правило.

## 3. Подготовка

1. Зафиксировать build/commit, с которого снят дамп.
2. Подключить плату и определить COM-порт.
3. Убедиться, что прошивка не запускает запись в `domain` во время дампа.
4. Использовать путь для дампа вне репозитория, например:

```powershell
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$dump = Join-Path $env:TEMP "domain-$stamp.bin"
```

5. Offset и размер брать из `crates/ha-app/partitions.csv`:

```text
domain: offset 0x880000, size 0x80000
```

Если partition table изменится, использовать фактический offset/size из
собранной таблицы, а не эти числа.

## 4. Снять полный дамп

Только чтение:

```powershell
$port = "COM5" # заменить на фактический
$offset = "0x880000"
$size = "0x80000"
$dump = Join-Path $env:TEMP "domain-$(Get-Date -Format 'yyyyMMdd-HHmmss').bin"

python -m esptool -p $port -b 460800 read_flash $offset $size $dump

Get-Item -LiteralPath $dump | Select-Object FullName, Length
Get-FileHash -LiteralPath $dump -Algorithm SHA256
```

Ожидаемый размер: `0x80000` = 524288 bytes.

Сохранить:

- путь к дампу;
- SHA256;
- вывод `esptool`;
- build/commit.

Дамп не коммитить в репозиторий.

## 5. Первичная классификация

Проверить первые байты в начале раздела и в начале second erase sector:

```powershell
$b = [IO.File]::ReadAllBytes($dump)
foreach ($off in 0, 0x1000, 0x2000, 0x40000) {
    $slice = $b[$off..([Math]::Min($off + 31, $b.Length - 1))]
    "{0:X6}: {1}" -f $off, (($slice | ForEach-Object { $_.ToString("X2") }) -join " ")
}
```

Ожидаемые признаки:

| Признак | Значение |
|---|---|
| `47 44 53 4D` (`MSDG`) в `0x0` или `0x1000` | RegionManager directory |
| `42 54 53 4D` (`MSTB`) в `0x0` или `0x40000` | old generic `FlashTable` |
| Всё `FF` в начале и в `0x1000` | вероятно пустой раздел |
| Иное | неизвестное состояние |

Не делать вывод только по одному magic: проверить header/CRC/records.

## 6. Разбор old generic layout

Текущий старый layout:

```text
bank size:       0x40000
header:          64 bytes, magic MSTB, persist_id "ha_domain"
record size:     16 + 21 + 458 + 8 = 503 bytes
capacity:        384
record magic:    RT
commit marker:   MSCM в последних 4 bytes record
key:             entity u32 + key_len u8 + key[16]
payload:         len u16 + record[456]
```

Read-only проверка:

```python
from pathlib import Path
import struct

path = Path(r"...\domain-....bin")
d = path.read_bytes()
assert len(d) == 0x80000

BANK = 0x40000
HDR = 64
SET_REC = 16 + 21 + 458 + 8
META_REC = 16 + 8
MSTB = b"BTSM"  # 0x4D535442 little-endian
RT = b"RT"

def u32(off):
    return struct.unpack_from("<I", d, off)[0]

for bank in (0, 1):
    base = bank * BANK
    hdr = d[base:base + HDR]
    print(f"bank {bank}: magic={hdr[:4].hex()} erased={all(x == 0xFF for x in hdr)}")
    if hdr[:4] != MSTB:
        continue
    print("  persist_id:", hdr[20:36].hex())

    counts = {}
    committed = 0
    offset = base + HDR
    while offset + 16 <= base + BANK:
        if d[offset:offset + 2] != RT:
            break
        kind = d[offset + 2]
        length = SET_REC if kind == 1 else META_REC if kind == 2 else 0
        if length == 0 or offset + length > base + BANK:
            break
        commit = u32(offset + length - 4)
        if commit != 0x4D53434D:
            break
        if kind == 1:
            entity = u32(offset + 16)
            counts[entity] = counts.get(entity, 0) + 1
            committed += 1
        offset += length

    print("  committed SET records:", committed)
    print("  entities:", dict(sorted(counts.items())))
```

Если `committed SET records > 0` — в старом layout есть данные. Нельзя
форматировать раздел до решения владельца данных.

## 7. Разбор RegionManager layout

Если `MSDG` найден:

- проверить оба directory slots (`0x0`, `0x1000`);
- выбрать валидный слот с большим generation;
- прочитать entries: `id[16]`, `offset u32`, `size u32`, `state u16`, CRC;
- перечислить `persist_id`/offset/size;
- не открывать и не изменять регионы.

Если directory валиден, но entries указывают на непустые регионы — считать,
что данные есть, до отдельного решения.

## 8. Классификация результата

Зафиксировать ровно один результат:

| Результат | Условие | Действие |
|---|---|---|
| `EMPTY` | обе directory/header области erased, записей нет | можно перейти к отдельному решению о сбросе |
| `OLD_EMPTY` | old generic header валиден, committed records = 0 | данных нет; решение о сбросе отдельно |
| `OLD_DATA` | old generic layout, committed records > 0 | **Stage 1 остановлен** |
| `REGION_DATA` | valid RegionManager с непустыми regions | **Stage 1 остановлен** |
| `UNKNOWN` | non-erased, layout не распознан / CRC ошибка | **fail-closed**, только диагностика |

`EMPTY` не означает автоматическое право форматировать: сброс `domain`
остаётся явным операторским действием.

Если оператор принял решение, что данные можно потерять, дальнейший порядок —
[DOMAIN_RESET_PROCEDURE.md](DOMAIN_RESET_PROCEDURE.md). Dump и SHA256 всё равно
снимаются до reset.

## 9. Что приложить к результату

- [ ] build/commit;
- [ ] COM-порт и команда чтения;
- [ ] путь к дампу;
- [ ] размер и SHA256;
- [ ] первые 32 байта `0x0`, `0x1000`, `0x40000`;
- [ ] классификация (`EMPTY`/`OLD_EMPTY`/`OLD_DATA`/`REGION_DATA`/`UNKNOWN`);
- [ ] для old layout — количество committed records и histogram entity;
- [ ] для RegionManager — список entries;
- [ ] решение: данных нет / данные есть / неизвестно.

## 10. Границы

- Процедура не является миграцией.
- Процедура не создаёт recovery protocol.
- Процедура не разрешает erase.
- Она только отвечает на вопрос: **есть ли в текущем `domain` данные,
  которые нельзя потерять.**
