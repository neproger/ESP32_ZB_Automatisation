# mstore — FLASH regions (contract)

> **Статус:** реализовано (revision 1): `mstore/src/storage/mstore_region.c`,
> host-тесты `test_regions`, проверено на ESP32-P4 rev 1.3.
> Это **не план реализации**: сигнатуры, типы и API уточняются в коде.
>
> Отклонения v1: состояние `RETIRED` не производится (регион не растёт, переразметки
> нет); `bind/release` не синхронизируются между задачами — concurrency отдельным
> шагом (§9 п.6).
>
> Связанные документы: архитектура примитивов — `MSTORE.md`; формат внутри региона —
> `MSTORE_FLASH_FORMAT.md`; статус и решения — `MSTORE_IMPL_JOURNAL.md`.

## 1. Зачем

Сегодня FLASH-backend отображает таблицу прямо на NOR-устройство:

```text
1 mstore FLASH table == 1 mstore_flash_device region
```

На устройстве один `mstore_flash_device` — весь раздел `esp_partition`, поэтому вторая
FLASH-таблица либо не открывается (identity не совпадает), либо пишет поверх первой.
Это блокирует Domain: Entity Store держит несколько таблиц одновременно
(`device`, `endpoint`, `automation`, `settings`, …).

Этот документ фиксирует слой, который разводит три ответственности:

```text
Table semantics        слоты, generation, version, индекс        — Table Engine
FLASH log format       банки, записи, CRC, commit marker         — FLASH backend
Region allocation      кто и где лежит в partition               — Region Manager
```

## 2. Границы ответственности

```text
esp_partition "mstore"        порт/platform: raw device (read/program/erase)
        ↓
Region Manager                owner partition: directory, allocated regions, binding
        ↓
region view                   derived device { base_offset, size, erase_size }
        ↓
FLASH backend                 внутри region: две банки, append-only log, checkpoint
        ↓
Table Engine                  семантика слотов
```

| Слой | Владеет | Не знает |
|---|---|---|
| Port (`port/espidf`) | raw device на раздел | про таблицы и регионы |
| **Region Manager** | directory, распределение регионов, правило bind/unbind | про слоты, записи, generation |
| region view | трансляция `offset + base_offset`, границы региона | про владение и directory |
| FLASH backend | формат внутри региона, recovery, checkpoint | про partition и соседние регионы |
| Table Engine | семантика | про физику |

Ключевое следствие: **FLASH backend продолжает считать регион своим и единственным** —
его код не меняется семантически.

## 3. Модель

```text
регион        = [base_offset, base_offset + size) внутри partition
persist_key   → ровно один регион (1:1)
persist_id    = тот же FNV-дубль, что уже используется в bank header
```

Правила:

1. `persist_key` ↔ region — взаимно однозначно.
2. Два разных `persist_key` **не пересекаются физически**: регион выровнен по
   `erase_size` и имеет размер, кратный `2 * erase_size`; erase-блоки не делятся между
   регионами и directory.
3. Повторное открытие того же `persist_key`, пока регион занят — **запрещено**:
   `MSTORE_INVALID_STATE`. Синхронизация двух владельцев не вводится.
4. Размер региона вычисляется **до** передачи backend'у; backend получает уже готовый
   регион и не занимается подбором геометрии.
5. Внутри региона backend по-прежнему использует свои две банки; их границы тоже
   erase-aligned (следует из размера региона).
6. `persist_id` в bank header остаётся дополнительной защитой от неправильного bind —
   Region Manager не является единственным контролем.

## 4. Размер региона

Region Manager получает геометрию таблицы и резервирует минимально необходимый кусок:

```text
max_record      = RECORD_FIXED + key_size + payload_size + RECORD_TAIL
records_needed  = capacity + APPEND_HEADROOM            # snapshot + запас под append
bank_content    = HEADER_SIZE + records_needed * max_record
bank_size       = round_up(bank_content, erase_size)
region_size     = 2 * bank_size
```

- Округление `bank_size` **вверх** до erase-блока: стирание банки не должно затрагивать
  соседний регион.
- Регион вмещает «capacity снимков + одну запись запаса» — ровно то, что уже требует
  формат (`MSTORE_FLASH_APPEND_HEADROOM`). Запас на будущий рост не резервируется.
- `round_up` с переполнением → `MSTORE_INVALID_SIZE`.

Пример (erase 4096, иллюстративные ёмкости Domain):

| Таблица | capacity | key | payload | bank | region |
|---|---|---|---|---|---|
| `device` | 64 | 8 | 128 | 12288 | 24576 |
| `endpoint` | 256 | 12 | 64 | 28672 | 57344 |
| `automation` | 32 | 8 | 1024 | 36864 | 73728 |
| `settings` | 16 | 8 | 256 | 8192 | 16384 |

Итого ~168 КБ + directory; раздел `mstore` в `partitions.csv` сейчас 0x40000 (256 КБ) —
хватает, но без запаса. Размер раздела нужно пересмотреть вместе с реальными schema
Domain (см. §9 п.7).

## 5. Directory регионов

Динамического allocator общего назначения **не будет**. Directory — bounded:

```text
partition
├── directory slot A      (1 erase block)
├── directory slot B      (1 erase block)
├── region 0              persist_key → offset/size
├── region 1
├── …
└── free tail
```

Запись directory (32 байта, выравнивание естественное):

```text
persist_id[16]  identity региона (тот же digest, что в bank header)
offset   u32    от начала partition, кратно erase_size
size     u32    размер региона, кратно 2 * erase_size
state    u16    FREE | USED | RETIRED
reserved u16
crc32    u32    поверх остальных полей записи
```

Геометрия таблицы (`capacity / key_size / payload_size`) в directory **не дублируется**:
её владелец — bank header внутри региона. Одно решение — один ответственный; при
расхождении диагноз даёт backend (`INVALID_STATE`), как сегодня.

Выделение региона — производное от directory, без отдельного free-list:

```text
free_offset = directory_end + Σ size(USED ∪ RETIRED)
```

Bump-аллокатор: новый регион занимает `free_offset`, дыры не переиспользуются (§9 п.4).

### 5.1. Долговечность directory

Directory — метаданные, поэтому обновление идёт ping-pong, два слота по одному
erase-блоку:

```text
содержимое нового слота собирается в RAM (bounded: header + N * 32)
erase неактивного слота
запись entries, затем заголовок с CRC — последним
старый слот НЕ стирается, пока новый не записан и не валиден
```

Recovery:

| Состояние | Исход |
|---|---|
| оба слота erased | fresh partition → создаётся пустая directory, generation = 1 |
| один слот валиден | используется он |
| два валидных | побеждает больший `generation` |
| валидного нет, но слот не erased | `MSTORE_CORRUPT` (без авто-формата) |

Такой порядок делает CORRUPT практически недостижимым: слот стирается только когда
второй слот уже хранит валидное предыдущее состояние.

## 6. Bind / release

```text
bind(persist_key, geometry)
  directory отсутствует      → создать
  запись есть, размер совпал  → отдать регион
  запись есть, размер меньше  → INVALID_SIZE (v1: регион не растёт)
  запись есть, регион занят   → INVALID_STATE
  записей нет, места хватает  → выделить из free tail, записать directory
  записей нет, directory полна→ NO_SPACE
  free tail меньше региона    → INVALID_SIZE

release(persist_key)          → снять признак занятости; reopen возможен
```

- Владение живёт в RAM (bounded по ёмкости directory); перезагрузка снимает все bind
  автоматически.
- `release` вызывается на `mstore_table_deinit`: тесты получают честный «reboot»,
  повторная инициализация той же таблицы — разрешённый сценарий.
- RETIRED-запись остаётся владельцем места до явной пересборки (§8).

## 7. Ошибки

Исходы те же, что у всего mstore: нейтральный `mstore_err_t`, без протекания
partition-специфики наружу. `NO_SPACE` означает «directory заполнена», `INVALID_SIZE` —
«геометрия не влезает», `INVALID_STATE` — «ключ уже связан», `CORRUPT` — «directory
повреждена». Сообщение не предлагает молча переразметить раздел: потеря данных не
должна быть тихой.

## 8. От чего отказываемся

- allocator общего назначения, дефрагментация, перемещение и сжатие регионов;
- рост региона на месте и «умное» переиспользование дыр;
- несколько partition на один mstore;
- шардинг одной таблицы по нескольким регионам;
- упаковка directory внутрь региона.

Всё это можно добавить отдельно, если появится доказаная потребность.

## 9. Открытые вопросы

1. ~~**Ёмкость directory.**~~ Решено: `N = 16` (`MSTORE_REGION_MAX_ENTRIES`). Следствие —
   требование `erase_size >= MSTORE_REGION_SLOT_BYTES` (528): у P4 4096.
2. ~~**Порядок появления регионов.**~~ Решено: lazy по первому bind, порядок фиксируется в
   directory.
3. ~~**Запас на рост.**~~ Решено: резерва нет; смена геометрии — диагноз, а не рост региона.
4. ~~**Политика смены геометрии.**~~ Решено: v1 — ошибка. Если новый размер региона совпал
   (обе ёмкости округляются до одного erase-блока), диагноз даёт bank header внутри
   региона — `INVALID_STATE`; иначе отказывает Region Manager — `INVALID_SIZE`.
5. **Несколько разделов.** Сейчас один `CONFIG_MSTORE_FLASH_PARTITION_LABEL`.
6. **Блокировки manager.** `bind/release` — общий singleton; защита и отношения с
    lock'ом таблицы — в рамках отдельного шага по concurrency.
7. **Размер раздела.** Пересчитать `partitions.csv` под реальные schema Domain.
8. ~~**Миграция.**~~ Решено: миграции нет. Переход на region manager требует стереть раздел —
   раздел с до-регионовой раскладкой даёт `MSTORE_CORRUPT` (без авто-формата), что и
   подтверждено на P4.

## 10. Порядок реализации (отдельные коммиты)

```text
1. контракт                     этот документ
2. directory + region view      host-тесты на NOR-sim (ping-pong, recovery, CORRUPT)
3. подключение к FLASH backend  вместо mstore_platform_flash_device()
4. multi-table тесты            две и более таблиц на одном разделе + reboot
5. пересчёт partitions.csv      по фактическим schema Domain
```

Шаги 1–4 выполнены и проверены на P4 (шаг 5 — вместе с реальными schema Domain).

Отдельный коммит по шагам 2–4 не выделялся: directory без подключения к backend'у не
имеет потребителя, поэтому directory, region view и подключение сделаны одним change'ем
с общим suite `test_regions`.
