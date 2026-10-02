# mstore — benchmark / performance characterization

> **Статус:** первый прогон выполнен на ESP32-P4 rev 1.3 (IDF 6.1, CPU 360 МГц,
> раздел `mstore` 4 МБ, erase block 4096). Цифры ниже — измеренные, не ожидаемые.
>
> Обвязка-бенчмарк и внутренние счётчики удалены вместе с `test_apps`; цифры
> сохранены как исторический замер. Формат отчёта был CSV-строки, печать только
> после сценария (внутри измеряемого цикла `printf` нет).

## 1. Что измеряем

```text
MODE,capacity,payload,operation,ops,total_us,ops_sec,avg_us,p50,p95,p99,max
```

Отдельно:

```text
MEMORY  — heap до/после init, стоимость init и fill, min free, largest block, leak delta
FLASH   — records_appended, compactions, program_calls/bytes, erase_calls/bytes
STRESS  — sustained ops/sec, consistent, min free, largest block
```

Сценарии: `fill/read` по capacity 100…5000; `checkpoint` (полная таблица, циклические
changed updates); `write_stress` (40% changed update / 30% read / 15% find /
10% free+allocate / 5% unchanged update) в двух режимах — isolated и под
фоновой CPU-нагрузкой; `soak` (длительная смесь с reopen и проверкой heap).

Процентили считаются по reservoir-выборке 1024 замера; min/max/total — по всем
операциям. Режимы RAM, FLASH и RAM|FLASH имеют разные `persist_key`, поэтому
занимают разные FLASH-регионы.

## 2. Результаты (headroom = 1 запись, то есть политика до правки)

Плата: ESP32-P4 rev 1.3, 360 МГц. Раздел `mstore` 4 МБ.

### RAM (capacity 1000, payload 32)

```text
find                5.3 µs (p99 6)
read                5.2 µs (p99 6)
allocate            6.5 µs
update_changed      5.2 µs
sustained (isolated) 118 000 ops/sec
sustained (loaded)    61 000 ops/sec   // фоновая задача того же приоритета: ~половина CPU
```

### FLASH (payload 32)

| capacity | find p50 | read p50 | update p50 | update p99 | update max |
|---|---|---|---|---|---|
| 100 | 51 µs | 188 µs | 527 µs | 131 585 µs | 132 722 µs |
| 1000 | 51 µs | 188 µs | 533 µs | 1 128 448 µs | 1 148 450 µs |
| 2000 | 51 µs | 189 µs | 533 µs | 1 876 732 µs | 1 906 729 µs |
| 5000 | 51 µs | 188 µs | 533 µs | 4 192 415 µs | 4 223 393 µs |

Стоимость одного сценария `fill/read` на capacity 5000:

```text
2000 update_changed   164.9 s
100 iter               91.3 s     // iter читает key каждого слота: при 5000 слотов это 5000 read
5000 free              19.0 s
reopen                  1.2 s
compactions             49
program_calls        514 049      (14.8 MB)
erase_bytes       29 704 192      (~29 MB)
```

Сценарий `checkpoint` (capacity 200, 5000 changed updates): `compactions = 1666`,
`erase_bytes = 40.9 MB`.

### RAM|FLASH (capacity 100, payload 32)

```text
find                5.7 µs     // чтение из RAM-зеркала
read                5.9 µs
update_changed p50  350 µs     // запись идёт write-through во FLASH
update_changed p99  127 581 µs // пик — compaction
```

### Память (heap, MALLOC_CAP_8BIT, ~404 КБ свободно)

| backing | capacity | cost_init | примечание |
|---|---|---|---|
| RAM | 100 | 7 980 B | index + free-list + scratch + mirror payload |
| RAM | 500 | 36 588 B | |
| RAM | 1000 | 72 972 B | |
| RAM | 2000 | 145 548 B | |
| RAM | 5000 | — | `NO_MEM`: internal RAM не вмещает, нужен PSRAM |
| FLASH | 100 | 4 556 B | |
| RAM\|FLASH | 100 | 8 992 B | RAM-зеркало + FLASH backend |

`leak_delta` после deinit — 0 для всех измеренных режимов.

## 3. Прогон с headroom = 25% (после правки sizing policy)

Политика запаса: `headroom = max(8, capacity * 25%)` (Kconfig
`MSTORE_FLASH_APPEND_HEADROOM_PERCENT` / `_MIN`). FLASH, payload 32, 360 МГц.

| capacity | headroom | update avg | update p99 | update max | compactions | erase bytes |
|---|---|---|---|---|---|---|
| 500 | 1 запись | — | — | — | 50 | 3 276 800 |
| 500 | 25% | 3 385 µs | 10 294 µs | 656 ms | 7 | 573 440 |
| 1000 | 1 запись | 51 134 µs | 1 128 448 µs | 1 148 ms | 99 | 12 165 120 |
| 1000 | 25% | 2 618 µs | 10 323 µs | 913 ms | 6 | 933 888 |

Что дала правка:

- sustained cost update упал примерно в 20 раз (51 134 → 2 618 µs на capacity 1000);
- erase-трафик — в 13 раз, число компакшенов — на порядок;
- `max` (одиночная compaction) практически не изменился: 913 ms против 1 148 ms.
  Запас уменьшает **частоту** compaction, а не стоимость одной: банка стала больше,
  поэтому одна compaction дороже. Bounded worst-case latency требует отдельного
  механизма (меньшие банки / шардинг), это не решается размером запаса.

Остальное на 25%: `find` p50 52 µs, `read` p50 189 µs, `free` p50 365 µs,
`reopen` 271 ms на capacity 1000. `leak_delta` после deinit — 0.

## 4. Что осталось незавершённым

- Свип `headroom` 10% / 50% / 100% начат, но остановлен: на FLASH один прогон идёт
  минуты, а раздел стирать между прогонами обязательно (размер региона зависит от
  политики, поэтому смена процента требует чистого раздела).
- Прогон останавливали «на ходу»: `esptool erase_region` во время работы
  предыдущей прошивки приводит к её panic (`SYS_CODE_CORRUPT` в ROM-пути) — стирать
  надо до перепрошивки, а не во время работы приложения.
- Плата: flash `boya` используется с generic-драйвером
  (`W (243) spi_flash: Detected boya flash chip...`) — для чистоты замеров стоит
  включить `SPI_FLASH_SUPPORT_BOYA_CHIP`.

## 5. Главный вывод первого прогона

Table Engine вне compaction быстрый и предсказуемый: FLASH `find` ~51 µs,
`read` ~188 µs, обычный append/update — сотни микросекунд. Узкое место —
**частота checkpoint**, а не сам движок.

При `MSTORE_FLASH_APPEND_HEADROOM = 1` полностью заполненная таблица деградирует:

```text
checkpoint -> snapshot capacity записей -> место ровно на 1 append
-> update -> банка снова полна -> следующий update -> снова checkpoint
```

Итог — перезапись порядка `O(capacity)` байт flash почти на каждый update и
worst-case latency до 4.2 с на capacity 5000. Это не дефект корректности
(все сценарии `consistent=1`), а плохая sizing policy.

Следующий шаг — политика `append_headroom = max(minimum, capacity * percent)`
и повторный прогон с меньшим числом дорогих сэмплов.
