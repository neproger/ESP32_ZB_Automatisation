# Статус

Где мы находимся, что проверено, что дальше. Карта системы — `ARCHITECTURE.md`;
контракты слоёв — в документах подсистем. Здесь только состояние работ.

## 1. Что сделано

| Компонент | Состояние | Проверено |
|---|---|---|
| `mstore` | Table + Ring, RAM/flash, region manager; заморожен (`storage/MSTORE_IMPL_JOURNAL.md`) | host-тесты 12/12, hardware verified на P4 |
| `sys` | единая модель ошибки `sys_error_t` (`ERRORS.md`) | тестами всех слоёв |
| `domain` | Entity Store, Journal, Transient Payload, Dispatcher, Commands; дешёвая ревизия записи `domain_entity_meta` (opaque version без payload) | host-тесты 9/9 (в т.ч. `meta`), IDF-сборка и запуск на P4 |
| `ha_model` | словарь ZCL, формы сущностей (device/state/endpoint/automation/group/group_item/location/weather), форма команды | `static_assert` + host-тестами zigbee |
| `zigbee` | задача сервиса, репорт → состояние, топология endpoint'ов, интервью, подписка (bind + Configure Reporting), события (raw ZCL → EVENT), executor и отправка команд, счётчики диагностики | host-тесты 4/4, запуск на P4 |
| `zigbee_radio` | spinel UART → RCP `ot_rcp` на C6 → стек Zigbee; комиссионирование, интервью, репорты и команды ZCL | живое устройство ESP32C6-DISPLAY: сеть, интервью, device + endpoint'ы в Domain |
| `automation` | подписка на EVENT (Zigbee + system) и на `ENTITY_UPSERTED`/`STATE`, правила (entity `automation`): триггеры `DEVICE_EVENT`, `TIME` (будильник + дни недели) и `STATE` (порог атрибута + фронт), условия (AND, в т.ч. оператор «содержит биты»), `domain_post` команды | host-тесты (правило/время/STATE) 1/1, сквозной цикл на P4, TIME-правило сохраняется (`kind/min/mask`) |
| `system` | сервис времени и погоды: SNTP + GeoIP (пояс/город, `ip-api`) + Open-Meteo (`current`, WMO→condition); синтетический девайс «Время», сущности `location` и `weather`; события-тики и `WEATHER_CHANGED` | запуск на P4: `sntp sync: ESP_OK`, tz/город, `weather: cond=2 t=10.5C`, snapshot с девайсом/состояниями/location/weather |
| `wifi` | сервис Wi-Fi: владелец радио на внешнем C3 (ESP-Hosted); подъём стека, скан, подключение, автоподключение по известным; сущности `wifi_scan/known/status`, команды `HA_CMD_WIFI_SCAN/CONNECT` | host-тест 1/1 (`wifi_select`), IDF-сборка; на железе не проверено (нет P4) |
| `web` | HTTP+WS (BFF) через внешний C3 (ESP-Hosted UART); бинарный протокол v2 (`services/WEB_PROTOCOL.md`): snapshot, дельта через Domain, команды (Zigbee, CRUD автоматизаций, переименование, устройство на удаление, permit-join); UI (`web-ui`) встроен в прошивку; радио не владеет | устройство отдаёт UI по `/`, `GET /`→200, snapshot и WS-команды проверены |
| `display` | UI на LVGL 9: экран группы (шапка + скролл-список виджетов), строка состояния на `lv_layer_top` (время/город/погода), бургер-меню, экран Wi-Fi (сети + диалог пароля), экран настроек (подсветка, скринсейвер), навигационные точки; кириллические шрифты, иконки погоды; host-превью с фейковым Domain | IDF-сборка `display` без предупреждений; host-превью (LVGL Live Preview) |
| `display_p4` | порт Display под плату: ST7701 480×800 MIPI-DSI (DSI bus + DBI io + DPI panel, init вручную), GT911 touch (I2C), esp_lvgl_port; `display_start` из `app_main` | запуск на P4: PSRAM найден, GT911 на 0x5D, LVGL task, порт стартовал (визуальная проверка — за пользователем) |
| `ha_p4` (приложение) | bootstrap: 13 типов сущностей (device/state/endpoint/automation/device_remove/location/group/group_item/weather/wifi_scan/wifi_known/wifi_status/settings), задача диспетчера, журнал в консоль | запуск на P4 rev 1.3 |

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
системный сервис      → SNTP sync; GeoIP пояс/город; девайс «Время» + `location` в Domain
системный тик         → ENTITY_UPSERTED состояний времени + EVENT (MINUTE_TICK) каждую минуту
TIME-триггер          → правило «будильник» сохраняется (kind=1, minutes_of_day, weekday_mask)
STATE-триггер         → состояние атрибута (в union записи, размер 144) + фронт rising/falling
```

Платы хватает на сборку и наблюдение: `idf.py build flash monitor` из корня проекта.

## 3. Что осталось заглушкой

```text
нет                      источник кадров (zigbee_stub_feed.c) удалён; репорты, интервью
                         и команды идут настоящим радиоканалом
wifi provisioning        сервис `wifi` (скан/connect/автоподключение) — provisioning только из
                         UI Display; на железе наблюдалась нестабильность AUTH_EXPIRE (AP/C3)
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
1. device_meta (last_seen/rssi/lqi)
2. Отдельный девайс «Система» — служебные вещи (uptime, версия) при старте
3. Аудио ES8311 / microSD — остальные периферии платы
```

Правила Automation создаются из UI (CRUD); поддержаны триггеры `DEVICE_EVENT`/`TIME`/
`STATE` и условия. Порог `STATE` уложился в запись **без** изменения размера (144):
поля триггера — `union` по видам, `persist_key`/миграция mstore не требуется.

Порядок прежний: Zigbee — единственный источник состояния, Automation и Web
потребляют то, что он производит.

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
- **Ассоциация C3 с AP.** На старте бывают `reason=2` (AUTH_EXPIRE) / `reason=201`
  (NO_AP_FOUND) при хорошем RSSI, потом подключается. Похоже на AP (band steering /
  защита) и/или помехи/просадку 5V от подозрительно горячего C6-RCP (греется до 90 °C).
  C6-RCP унесён на отдельное питание; разбирается.
- **UART P4↔C3 (esp_hosted).** Транспорт — только software flow control, без checksum;
  вышe 115200 (`460800`/`921600`) — `bring-up timed out`. Смягчено: очереди 24 на обоих
  концах, LVGL на ядре 1. Диагностика потерь — `[frame] v1 bad offset` в логе P4.
- **Concurrency Region Manager.** `bind/release` не синхронизированы между задачами
  (`storage/MSTORE_FLASH_REGIONS.md` §9.6).
- **Размер раздела.** Ёмкости и расчёт — `storage/MSTORE_FLASH_REGIONS.md` §4 (≈120 КБ из
  256 КБ с группами); пересчёт под реальное число устройств/групп — `services/ZIGBEE.md` §10.
- **Экраны Display.** Проекция `(cluster, attr) → widget`, порядок экранов и способ чтения
  списка виджетов — открыто в `clients/DISPLAY.md` §7.

## 6. Как собирать и проверять локально

```text
приложение на P4   — idf.py build flash monitor (из корня esp32-p4_HA_v2)
domain host-тесты  — domain/tests/README.md
mstore host-тесты  — те же шаги для mstore/tests
zigbee host-тесты  — те же шаги для zigbee/tests
wifi host-тесты    — те же шаги для wifi/tests (test_wifi_select)
UI превью (без P4) — display/preview/README.md (LVGL Live Preview)
```

Все наборы обязательны перед коммитом: `/W4` на MSVC и `-Wall -Wextra -Werror` на
GCC/Clang, предупреждения недопустимы.

**Нюанс окружения.** `export.ps1` по умолчанию подхватывает python-окружение от
IDF 5.5 и падает. Рабочий запуск — с явным
`IDF_PYTHON_ENV_PATH=...\.espressif\python_env\idf6.1_py3.10_env`.
