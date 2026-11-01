# Явный сброс раздела `domain` перед Stage 1

> Статус: **destructive procedure по отдельному операторскому решению**.
> Не является автоматическим поведением приложения.
>
> Решение: содержимое `domain` можно потерять. Перенос старых данных не
> выполняется.
>
> Связанные документы:
> [DOMAIN_PARTITION_DIAGNOSTIC.md](DOMAIN_PARTITION_DIAGNOSTIC.md),
> [DOMAIN_PERSISTENCE_AUDIT.md](DOMAIN_PERSISTENCE_AUDIT.md).

## 1. Жёсткие условия

Не выполнять, пока одновременно не выполнено:

- [ ] read-only dump `domain` снят;
- [ ] SHA256 дампа сохранён;
- [ ] оператор явно зафиксировал: данные в `domain` можно потерять;
- [ ] Stage 1 firmware собран и готов к прошивке;
- [ ] известно, что старый firmware не будет загружен после erase;
- [ ] `mstore` и остальные разделы не затрагиваются.

`RegionManager` не должен стирать раздел при несовпадении layout. Сброс —
только эта отдельная операция.

## 2. Почему сначала Stage 1 firmware

Если стереть `domain`, а затем загрузить **старый** firmware, он снова создаст
old generic layout. Поэтому erase и прошивка Stage 1 должны быть одним
операторским окном:

```text
dump → decision → erase domain → flash Stage 1 → boot Stage 1
```

Не оставлять устройство загружаться со старым firmware между erase и flash.

## 3. Dump перед изменением

Использовать read-only процедуру:

[DOMAIN_PARTITION_DIAGNOSTIC.md](DOMAIN_PARTITION_DIAGNOSTIC.md)

Сохранить:

- build/commit;
- путь к дампу;
- размер `0x80000`;
- SHA256;
- классификацию layout.

Даже при решении «данные не нужны» dump остаётся доказательством состояния
до изменения.

## 4. Зафиксировать решение

Записать отдельно (в задаче/журнале, не в коде):

```text
Дата:
Оператор:
Build до сброса:
Dump:
SHA256:
Решение: данные domain можно потерять.
Причина: Stage 1 переходит на V2 Domain persistence; миграция не выполняется.
```

## 5. Явно стереть только `domain`

Offset/size брать из `crates/ha-app/partitions.csv`:

```text
domain: offset 0x880000, size 0x80000
```

Команда:

```powershell
$port = "COM5" # заменить
python -m esptool -p $port erase_region 0x880000 0x80000
```

Запрещено:

- `erase_flash`;
- `idf.py erase-flash`;
- стирание `mstore` (`0x810000`, `0x60000`);
- изменение partition table;
- автоматический erase при `RegionManager` layout mismatch.

`mstore` используется Zigbee Registry и не должен пострадать.

## 6. Проверить, что раздел пуст

Прочитать начало раздела:

```powershell
$after = Join-Path $env:TEMP "domain-after-erase.bin"
python -m esptool -p $port read_flash 0x880000 0x1000 $after
```

Проверить, что первые 64 bytes — `FF`, а также что дамп имеет размер `0x1000`.
Полный повторный dump `0x80000` — по желанию, для протокола.

## 7. Прошить Stage 1 и проверить

1. Прошить Stage 1 firmware штатным build-процессом.
2. Загрузить устройство.
3. Проверить логи:
   - `RegionManager` не сообщает `Corrupt` на `domain`;
   - Domain storage открывается;
   - restore не публикует facts;
   - persistent tables пусты на первом запуске.
4. Записать persistent-запись (например Settings/Group).
5. Перезагрузить.
6. Проверить, что запись восстановлена.
7. Проверить, что `PROPERTY_STATE`, Weather, Wi-Fi Scan/Status после reboot
   пусты.
8. Проверить Zigbee Registry в `mstore` не пострадал.

## 8. Rollback

Дамп сохранён, но автоматический rollback не предусмотрен: старый layout и
новый RegionManager несовместимы. Восстановление старого дампа имеет смысл
только вместе со старым firmware и только как отдельное решение. Это не
часть Stage 1 и не migration-протокол.

## 9. Что не входит

- перенос старых записей;
- migration markers;
- recovery state machine;
- автоматический erase при несовпадении layout;
- изменение `mstore`;
- startup/periodic compaction;
- изменение Zigbee-слоя.
