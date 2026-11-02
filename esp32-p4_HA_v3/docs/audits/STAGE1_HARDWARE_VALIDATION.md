# Stage 1 — hardware validation

> Статус: **инфраструктура Stage 1 проверена на P4; уникальный
> application-level persistence test ещё не выполнен.**
> Порт: COM11. MAC: `80:f1:b2:d1:18:77`.
> Source: `e98d6b4`; firmware собран `tools/build-ha-app.ps1`.

## 1. Dump до изменений

```text
offset: 0x880000
size:   0x80000
file:   C:\Users\ASUS\AppData\Local\Temp\domain-20261102-195607.bin
sha256: BD7FC7CFE84B18B2E427504B2A1B942B94A9F524346A4FA703813AE4A6F11B04
```

Классификация: **OLD_DATA**.

- bank0: `MSTB`, активный, 13 committed SET records;
- bank1: erased;
- entity histogram: `6` (Location) ×1, `13` (Settings) ×1, `14` (Entity) ×4,
  `16` (Capabilities) ×6, `17` (Logical Device) ×1;
- RegionManager directory (`MSDG`) отсутствовал.

Данные были объявлены disposable; migration не выполнялась.

## 2. Erase и flash

Выполнено явно, только `domain`:

```powershell
espflash erase-region --port COM11 --after no-reset --non-interactive 0x880000 0x80000
espflash flash --port COM11 --baud 921600 --before no-reset --after hard-reset \
  --partition-table crates/ha-app/partitions.csv --non-interactive \
  C:/t/hav3/riscv32imafc-esp-espidf/debug/ha-app
```

`mstore` не стирался. `erase_flash`/`idf.py erase-flash` не использовались.
Старый firmware после erase не загружался: erase оставил chip в bootloader,
flash выполнен до reset.

## 3. Первый запуск Stage 1

Boot logs:

```text
registry ready: mstore=393216 bytes entries=1 bank=0 seq=1
zb_poc: stack started, waiting for network
registry: restored profile=tuya.ts0041 (no interview)
ha-app ready: device table + journal, waiting for button events
```

Domain storage открылся, `Domain::restore()` не завершил startup ошибкой.
Zigbee Registry в `mstore` сохранился (`entries=1`).

## 4. Domain layout после Stage 1

Directory `domain` прочитан read-only:

```text
slot 0: MSDG, generation=7
slot 1: MSDG, generation=6
```

Активен slot 0. Семь регионов соответствуют V2 keys:

| persist_key | offset | size |
|---|---:|---:|
| `group` | 0x2000 | 0x2000 |
| `group_item_v3` | 0x4000 | 0x6000 |
| `group_auto_v1` | 0xa000 | 0x2000 |
| `automation_v5` | 0xc000 | 0xa000 |
| `location` | 0x16000 | 0x2000 |
| `wifi_known` | 0x18000 | 0x2000 |
| `settings` | 0x1a000 | 0x2000 |

`Logical Device`, `Entity`, `Capabilities` в Domain flash отсутствуют — они
RAM, как в V2.

## 5. Reboot и сохранность региона

Settings region read before/after hard reset:

```text
before: 61BFFE5E577EE4C6EF0A9A345DA474DD632D4097C26AA54AADE3BA5F249AED27
after:  61BFFE5E577EE4C6EF0A9A345DA474DD632D4097C26AA54AADE3BA5F249AED27
```

Settings region содержит committed SET record (`generation=1`, `version=1`) и
после reboot байт-идентичен. Это подтверждает, что flash-регион не пересоздан
и данные пережили reset.

Ограничение: это default Settings record, а не уникальное значение,
введённое через UI/Web. Поэтому byte-identical reboot test не отличает
«restore существующей записи» от «повторный seed того же значения».
Уникальный application-level тест остаётся обязательным.

## 6. Что ещё не проверено

- уникальная запись через Web/Display → reboot → чтение;
- web/display snapshot после restore;
- поведение при ошибке storage во время работы;
- полный V2 scenario suite.

Причина: на устройстве нет известной Wi-Fi сети (`no known networks`), Web
недоступен; UI-запись не автоматизирована. Аппаратная проверка Stage 1 пока
считается **частичной**, не финальной.

## 7. Следующий шаг

1. Подключить устройство к Wi-Fi или использовать Display для создания
   уникального Group/Settings record.
2. Перезагрузить.
3. Проверить, что уникальное значение восстановлено.
4. После этого Stage 1 можно считать проверенным на железе.
