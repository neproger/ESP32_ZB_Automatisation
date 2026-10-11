# System Service

Слой: **Services / System**. Карта системы — `../ARCHITECTURE.md`.

## 1. Роль

Сервис времени и погоды. Держит **синтетическое системное устройство** в Domain: точное
время (SNTP), локальный пояс и город (GeoIP) и текущую погоду (Open-Meteo).

Устройство не Zigbee: оно живёт в обычных сущностях Domain (`device` / `endpoint` /
`state` / `location`), поэтому Automation и Web видят его как любое другое. Сеть сервис
сам не поднимает — ждёт появления IP на STA-интерфейсе, который поднял Web-сервис
(Wi-Fi через сопроцессор C3, ESP-Hosted).

## 2. Основной сценарий

```text
system_start(domain) -> task:
  ждём сеть (IP на WIFI_STA_DEF)
  SNTP (pool.ntp.org) -> системные часы
  GeoIP (ip-api.com) -> пояс + город -> TZ (POSIX), сущность location
  Open-Meteo -> HA_ENTITY_WEATHER (WMO code -> condition); смена условия -> WEATHER_CHANGED
  создаём device/endpoint системного девайса
  далее каждую минуту:
    обновляем состояния времени
    публикуем события-«тики»
  раз в 15 минут обновляем погоду
  спим до начала следующей минуты
```

## 3. Системная Entity

Id — `HA_SYSTEM_DEVICE_ID` (`ha_model/ha_system.h`), ASCII "SYST" в старших байтах, вне
диапазона реальных Zigbee EUI-64. System публикует семантику своих величин **напрямую** в
canonical `PROPERTY_STATE` `(ENTITY_SYSTEM, property)` — ZCL/кластеров не касается
(это единственный сервис, который не ходит через мост `zigbee/`).

Свойства (`ha_property_id_t`):

```text
SYSTEM_TIME_UTC, SYSTEM_TIME_STATUS,
SYSTEM_HOUR, SYSTEM_MINUTE, SYSTEM_WEEKDAY, SYSTEM_WEEKDAY_MASK,
SYSTEM_MINUTES_OF_DAY, SYSTEM_TZ_OFFSET
```

Строка города в `ha_value_t` не влезает (значение — число/enum), поэтому она — отдельная
сущность `HA_ENTITY_LOCATION` (текст + координаты + tz-offset), ключ — `HA_SYSTEM_DEVICE_ID`.

### 3.1. Погода

Погода — отдельная сущность `HA_ENTITY_WEATHER`, форма — `ha_model/ha_weather.h`, ключ —
синтетический `HA_WEATHER_DEVICE_ID` (вне диапазона Zigbee). Одна запись на текущие
условия: `condition` (enum; имена как в HA и в иконках v1), температура/влажность в 0.01,
давление, скорость и направление ветра, облачность. RAM: данные приходят из сети,
персистентность не нужна (как `location`). Заполняет сервис: Open-Meteo `current`
(`temperature_2m`, `relative_humidity_2m`, `weather_code`, `wind_speed_10m`,
`wind_direction_10m`, `cloud_cover`, `surface_pressure`, `is_day`), перевод WMO
`weather_code` → `condition`. Временных меток в записи нет — они ломали бы свёртку
(`../RECORD_MODEL.md` §2.2).

## 4. События (для автоматизаций)

Публикуются фактом `EVENT` (`domain_payload_put`, `value.enum` = id), source `SYSTEM`.
Automation подписан и на `SYSTEM`, поэтому правило вешается на пару (uid, id):

| id | событие |
|---|---|
| 1 | `MINUTE_TICK` — каждую минуту |
| 2 | `HALF_HOUR_TICK` — каждые 30 минут (`:00`/`:30`) |
| 3 | `HOUR_TICK` — каждый час |
| 4 | `DAY_TICK` — 00:00 локального времени |
| 5 | `WEATHER_CHANGED` — сменилось условие погоды |

Так выражаются типовые расписания:

- **каждые 30 минут** — триггер `HALF_HOUR_TICK` (без условий);
- **в 7:00** — триггер `MINUTE_TICK` + условие `Минуты от начала суток = 420`;
- **дни недели** — условие `Маска дней недели` с оператором **«содержит биты»**
  (`HA_CONDITION_OP_HAS_BITS`), напр. значение `0x1F` — Пн–Пт.

## 5. Реализация

Компонент `system` (`system/src/system.c`). Зависит от `domain`/`ha_model`/`sys`; из
IDF — `esp_netif` (SNTP + IP STA), `esp_http_client` (GeoIP + Open-Meteo). Внешнего
JSON-парсера нет: `json_find`/`json_string`/`json_number` ищут поля по имени. У Open-Meteo
ключи дублируются в `current_units` (строки), поэтому поля берутся из подстроки после
`"current":`. IANA-имена поясов newlib не знает — TZ строится из смещения (знак в POSIX
TZ обратный).
