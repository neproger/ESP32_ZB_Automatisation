# Статус

Где мы находимся, что проверено, что дальше. Карта системы — `ARCHITECTURE.md`;
контракты слоёв — в документах подсистем. Здесь только состояние работ.

> **Baseline.** Завершены Steps A–F: canonical Device/Entity-модель введена, Zigbee —
> полностью внутренний адаптер. Дальнейшие изменения — от продуктового кейса или проблемы
> на железе, а не от «очищения слоёв».

## 1. Что сделано

| Компонент | Состояние | Проверено |
|---|---|---|
| `mstore` | Table + Ring, RAM/flash, region manager; заморожен (`storage/MSTORE_IMPL_JOURNAL.md`) | host-тесты 12/12, hardware на P4 |
| `sys` | `sys_error_t`; `sys_layer_t` — opaque (core-слои), adapter-слои у себя (`ERRORS.md`) | тестами всех слоёв |
| `domain` | Entity Store, Journal, Transient Payload, Dispatcher, Commands; `domain_entity_meta`; `source` — opaque-число | host-тесты 9/9 |
| `ha_model` | canonical словарь: `Entity`, `Logical Device`, `Property`, `Capability`, `Event`, `Action`, `Rule`, `Group`, `Settings`; opaque `source`/`issuer`; `ha_value_to_double` | `static_assert` + host-тестами |
| `device` | generic Device service: `register/bind/rename/remove` + `register_remover(source,…)`; транспорт-нейтрален | host-тест 1/1 |
| `zigbee` | адаптер: приватный RAM-store устройства/топологии, интервью, bindings/reporting, мост ZCL↔canonical, события, executor `HA_CMD_PROPERTY`, remove (deferred leave, NVS), radio spinel→`ot_rcp` | host-тесты 10/10, запуск на P4 |
| `automation` | подписка на canonical EVENT/STATE (по типу факта); правила `ha_rule_t` (entity-native): списки триггеров (до 4) `DEVICE_EVENT`/`TIME`/`STATE` и действий (до 4), условия (AND), `domain_post HA_CMD_PROPERTY` | host-тест 1/1, сквозной цикл на P4 |
| `system` | время/погода: SNTP + GeoIP + Open-Meteo; публикует canonical `PROPERTY_STATE` системной Entity напрямую (ZCL не касается) + тики | запуск на P4 |
| `wifi` | владелец радио на C3 (ESP-Hosted): стек, скан, подключение; сущности `wifi_scan/known/status` | host-тест 1/1; на железе — provisioning/стабильность AP |
| `web` | HTTP+WS транспорт canonical-модели, byte-wire v3 (records verbatim, `services/WEB_PROTOCOL.md`): snapshot, дельты, команды; UI встроен | устройство отдаёт UI, snapshot/команды |
| `display` | LVGL 9: группы/виджеты, status bar, меню, Wi-Fi, настройки; читает canonical | IDF-сборка, host-превью |
| `ha_p4` (приложение) | bootstrap: 13 canonical типов (automation, location, group, group_item, weather, wifi_scan, wifi_known, wifi_status, settings, entity, property_state, capabilities, logical_device) | запуск на P4 rev 1.3 |

## 2. Что проверено на плате (ESP32-P4 rev 1.3, IDF 6.1)

```text
радиоканал            → spinel UART GPIO14/15 ↔ C6 20/21 → RCP ot_rcp (802.15.4)
Wi-Fi (внешний C3)    → ESP-Hosted SPI 40 МГц → got ip + веб-сервер
интервью              → Simple_Desc → Basic → Logical Device + Entity(1 на endpoint) + Capabilities
состояние             → репорт → canonical PROPERTY_STATE (entity, property)
события               → client OnOff → canonical EVENT (press) → Automation
команда от UI         → HA_CMD_PROPERTY → adapter резолвит entity→addr, кодирует ZCL → репорт
automation            → кнопка/время/состояние → правило → COMMAND_SENT → репорт
удаление устройства   → LOGICAL_DEVICE_REMOVE → Device service → remover → leave (best-effort)
тайм-сервис           → SNTP + GeoIP + погода → canonical PROPERTY_STATE системной Entity
web wire v3           → ENTITY/DEVICE/PROPERTY_STATE/CAPABILITIES/EVENT/GROUP_ITEM
                         + LOCATION/WEATHER/WIFI_STATUS/SETTINGS/GROUP (entity-native)
```

## 3. Что осталось заглушкой / известные проблемы

```text
wifi provisioning        только из UI Display; на железе нестабильность AUTH_EXPIRE (AP/C3)
network leave на железе  роутер ESP32C6-DISPLAY переподключается и переинтервьюируется;
                         снятие состояния проверено host-тестом
composition              зафиксирован инвариант: 1 native Device → 1 Logical Device,
                         1 endpoint → 1 Entity; child Device/агрегация endpoint'ов — отложены
                         до конкретного устройства с явно неверной структурой
словарь возможностей     расширять только под реальные устройства (IAS Zone, Shutter,
                         Thermostat, Metering/Electrical и т.п.)
```

## 4. Что дальше

```text
1. Реальные устройства, для которых текущая модель/словарь даёт неверный результат
2. device_meta (last_seen/rssi/lqi) — когда появится потребитель
3. Аудио ES8311 / microSD — остальные периферии платы
```

## 5. Открытые вопросы (где решение, а не здесь)

- **`device_meta`.** Отдельный тип сущности или поля Logical Device (`RECORD_MODEL.md`).
- **Composition.** Правило «endpoint'ы/кластеры → Entity/child Device» — `ARCHITECTURE.md`
  §1.4 / `services/ZIGBEE.md`; отложено.
- **`PERMIT_JOIN`.** Сейчас `HA_CMD_DISCOVERY` (generic); adapter трактует как permit join.
- **Concurrency Region Manager.** `bind/release` не синхронизированы (`storage/MSTORE_FLASH_REGIONS.md`).
- **Ассоциация C3 с AP.** `reason=2` (AUTH_EXPIRE) / `reason=201` при хорошем RSSI; разбирается.

## 6. Как собирать и проверять локально

```text
приложение на P4   — idf.py build flash monitor (из корня esp32-p4_HA_v2)
web-ui             — npm run build в web-ui/ (ПЕРЕД idf.py build: web/ui вшивается)
host-тесты         — domain/ ha_model/ semantics(в zigbee)/ automation/ device/ zigbee/ wifi/
```

Все наборы обязательны перед коммитом: `/W4` на MSVC и `-Wall -Wextra -Werror` на
GCC/Clang, предупреждения недопустимы.

**Нюанс окружения.** `export.ps1` по умолчанию подхватывает python-окружение от IDF 5.5 и
падает. Рабочий запуск — с явным `IDF_PYTHON_ENV_PATH=...\python_env\idf6.1_py3.10_env`.
