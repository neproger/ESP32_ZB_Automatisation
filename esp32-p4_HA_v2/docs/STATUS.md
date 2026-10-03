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
| `zigbee` | задача сервиса, репорт → состояние, топология endpoint'ов, интервью, подписка (bind + Configure Reporting), события (raw ZCL → EVENT), executor и отправка команд, счётчики диагностики | host-тесты 4/4, запуск на P4 |
| `zigbee_radio` | spinel UART → RCP `ot_rcp` на C6 → стек Zigbee; комиссионирование, интервью, репорты и команды ZCL | живое устройство ESP32C6-DISPLAY: сеть, интервью, device + endpoint'ы в Domain |
| `automation` | подписка на EVENT, правила (entity `automation`), `domain_post` команды | host-тест 1/1, сквозной цикл на P4 |
| `web` | Wi-Fi STA через внешний C3 (ESP-Hosted UART), HTTP + бинарный WS (каркас) | Wi-Fi `got ip`, веб-сервер стартует на P4 |
| `ha_p4` (приложение) | bootstrap: типы и ёмкости, задача диспетчера, журнал в консоль | запуск на P4 rev 1.3 |

## 2. Что проверено на плате (ESP32-P4 rev 1.3, IDF 6.1, 360 МГц)

```text
топология endpoint'а  → ENTITY_UPSERTED entity=3
репорт атрибута       → ENTITY_UPSERTED entity=2 (отдельный факт на атрибут)
команда от UI         → COMMAND_SENT с адресатом (entity=1, key=uid)
устройство            → пишется во flash один раз, переживает перезагрузку
канал диагностики     → zigbee.diag, отдельно от журнала фактов
радиоканал            → spinel UART GPIO29/30 460800 → RCP ot_rcp на C6 поднят
C6 (ot_rcp)           → 802.15.4 (Zigbee); Wi-Fi вынесен на внешний C3
Wi-Fi (внешний C3)    → ESP-Hosted UART2 GPIO32/28, reset GPIO34, 115200 → got ip + веб-сервер
комиссионирование     → сеть сформирована, steering выполнен, открыта на 180 с
интервью              → Active_EP(3) → Simple_Desc(ep 242/2/1) → Basic "ESP32C6-DISPLAY"
                      → ENTITY_UPSERTED device=1 и три endpoint=3
подписка              → bind server-кластеров + Configure Reporting
                      → ENTITY_UPSERTED state=2 (Level, OnOff) от живого устройства
события               → кнопка ESP32C6-DISPLAY: EVENT (OnOff Toggle) с payload
automation            → кнопка → EVENT → правило → COMMAND_SENT → репорт состояния=2
```

Платы хватает на сборку и наблюдение: `idf.py build flash monitor` из корня проекта.

## 3. Что осталось заглушкой

```text
нет                      источник кадров (zigbee_stub_feed.c) удалён; репорты, интервью
                         и команды идут настоящим радиоканалом
```

Уход устройства — по сигналу `LEAVE_INDICATION`/`DEVICE_UPDATE` (снятие device,
endpoint'ов и состояния) проверен host-тестом; на железе сетевой leave устройства
(роутер ESP32C6-DISPLAY) не воспроизвёлся — оно переподключилось и переинтервьюировалось.
Явное удаление устройства из UI и явная операция permit-join — когда появится потребитель.

Отправка команд (`zigbee_radio_send`), приём репортов, комиссионирование и интервью —
настоящий код. Устройство без интервью живёт в RAM сервиса и в Domain не появляется:
репорт от него — не факт. Провал подъёма радио возвращается в bootstrap: `app_main`
останавливается, и `bootstrap done` не печатается — без Zigbee система не поднимается.

## 4. Что дальше

```text
1. Правила Automation из UI/Web     — вместо bring-up seed; CRUD правил
2. device_meta (last_seen/rssi/lqi) — операционные данные для потребителей
3. Явные операции устройством       — удаление пользователем, permit-join
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
- ~~**Устройство без интервью.**~~ Решено: устройство без интервью живёт в RAM сервиса,
  в Domain попадает по завершении интервью (`services/ZIGBEE.md` §9.4).
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
