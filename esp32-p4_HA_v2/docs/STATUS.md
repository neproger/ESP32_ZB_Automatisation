# Статус

Где мы находимся, что проверено, что дальше. Карта системы — `ARCHITECTURE.md`;
контракты слоёв — в документах подсистем. Здесь только состояние работ.

## 1. Что сделано

| Компонент | Состояние | Проверено |
|---|---|---|
| `mstore` | Table + Ring, RAM/flash, region manager; заморожен (`storage/MSTORE_IMPL_JOURNAL.md`) | host-тесты 12/12, hardware verified на P4 |
| `sys` | единая модель ошибки `sys_error_t` (`ERRORS.md`) | тестами всех слоёв |
| `domain` | Entity Store, Journal, Transient Payload, Dispatcher, Commands | host-тесты 9/9, IDF-сборка и запуск на P4 |
| `ha_model` | словарь ZCL, формы сущностей, форма команды | `static_assert` + host-тестами zigbee |
| `zigbee` | задача сервиса, репорт → состояние, топология endpoint'ов, executor и отправка команд, счётчики диагностики | host-тесты 3/3, запуск на P4 |
| `zigbee_radio` | ESP-Hosted по SDIO → RCP на C6 → стек Zigbee, репорты и команды ZCL | SDIO и C6 видны, ждёт прошивку RCP на C6 |
| `ha_p4` (приложение) | bootstrap: типы и ёмкости, задача диспетчера, журнал в консоль | запуск на P4 rev 1.3 |

## 2. Что проверено на плате (ESP32-P4 rev 1.3, IDF 6.1, 360 МГц)

```text
топология endpoint'а  → ENTITY_UPSERTED entity=3
репорт атрибута       → ENTITY_UPSERTED entity=2 (отдельный факт на атрибут)
команда от UI         → COMMAND_SENT с адресатом (entity=1, key=uid)
устройство            → пишется во flash один раз, переживает перезагрузку
канал диагностики     → zigbee.diag, отдельно от журнала фактов
радиоканал            → SDIO 4-bit CLK=18 CMD=19 D0..D3=14..17 RESET=54 поднят
C6 по SDIO            → esp32c6, host fw 3.0.9; RCP запрошен, но CP fw 2.3.2 его не знает
```

Платы хватает на сборку и наблюдение: `idf.py build flash monitor` из корня проекта.

## 3. Что осталось заглушкой

```text
прошивка C6              на нём стоковый Wi-Fi-slave 2.3.2 без RCP. CP-прошивка
                         (ESP-Hosted 3.0.9 + FEAT_OPENTHREAD) уже собрана; C6
                         шьётся внешним 3.3 В USB-TTL в JP1 — у C6 нет своего USB
                         (процедура: services/ZIGBEE_IMPL_JOURNAL.md §5)
источник кадров          zigbee_stub_feed.c — больше не стартует, удалить после первого устройства
интервью устройства      нет: топология публикуется stub_feed
```

Отправка команд (`zigbee_radio_send`) и приём репортов идут настоящим кодом; пока
C6 не перепрошит, `zigbee.radio` логирует «RCP on the co-processor not started».

## 4. Что дальше

```text
1. Прошить C6 CP-прошивкой через USB-TTL в JP1   — снимает блокер радиоканала
2. Отладка на живом устройстве      — сеть, репорты, команды; затем удалить stub_feed
3. Automation service               — подписчик фактов, правила по состоянию
4. Web service (BFF)                — проекция записей в DTO, приём команд
```

Порядок прежний: Zigbee — единственный источник состояния, Automation и Web
потребляют то, что он produces.

## 5. Открытые вопросы (где решение, а не здесь)

- **`device_meta`.** Отдельный тип сущности или поля внутри `device`
  (`RECORD_MODEL.md` §10.3). Operational metadata (last_seen, rssi, lqi) пока не
  хранится нигде.
- **Гранулярность дискриминатора команды.** Сейчас один `HA_CMD_ZIGBEE_CLUSTER` на
  весь кластерный путь (`COMMANDS.md` §8).
- **Устройство без интервью.** Сейчас запись устройства создаётся первым репортом,
  до завершения интервью (`services/ZIGBEE.md` §9.4).
- **Фильтр по ключу в Dispatcher'е.** Подписчик фильтрует по устройству сам, сравнивая
  ключ; фильтр в самом Dispatcher'е не вводился — ждёт потребителя (`DISPATCHER.md` §5).
- **Concurrency Region Manager.** `bind/release` не синхронизированы между задачами
  (`storage/MSTORE_FLASH_REGIONS.md` §9.6).
- **Размер раздела.** Под реальное число устройств не пересчитывался: сейчас занято
  16 КБ из 256 КБ, расчёт — `services/ZIGBEE.md` §10.

## 6. Как собирать и проверять локально

```text
приложение на P4   — idf.py build flash monitor (из корня esp32-p4_HA_v2)
domain host-тесты  — domain/tests/README.md
mstore host-тесты  — те же шаги для mstore/tests
zigbee host-тесты  — те же шаги для zigbee/tests
```

Все наборы обязательны перед коммитом: `/W4` на MSVC и `-Wall -Wextra -Werror` на
GCC/Clang, предупреждения недопустимы.

**Нюанс окружения.** `export.ps1` по умолчанию подхватывает python-окружение от
IDF 5.5 и падает. Рабочий запуск — с явным
`IDF_PYTHON_ENV_PATH=...\.espressif\python_env\idf6.1_py3.10_env`.
