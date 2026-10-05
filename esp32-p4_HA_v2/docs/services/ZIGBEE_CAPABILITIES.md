# Zigbee: интерпретация устройств и проекция возможностей

Слой: **Services / Zigbee** (интерпретация) → потребители (**Web / Automation**).
Карта системы — `../ARCHITECTURE.md`; формы записей — `../RECORD_MODEL.md`,
`ha_model/ha_entities.h`; словарь ZCL — `ha_model/include/ha_model/ha_zigbee.h`.

Документ отвечает на вопрос: **по каким данным система понимает, что перед ней за
устройство** (реле, кнопка, датчик, диммер, замок), и как это единообразно
проецируется в «возможности» для потребителей.

Значения идентификаторов и семантика ролей сверены по двум независимым источникам —
спецификациям Zigbee (см. §8) и заголовкам стека `esp-zigbee-lib`
(`ezbee/...`); совпадение обязательно, расхождение — ошибка здесь, а не в стеке.

## 1. Что объявляет устройство

Устройство описывает себя через **endpoint'ы**. Каждый endpoint даёт **Simple
Descriptor** (ZDO `Simple_Desc_req`):

```text
profile_id
device_id
app_input_cluster_list     — server-кластеры
app_output_cluster_list    — client-кластеры
```

Стек: `ezbee/af.h` — `ezb_af_simple_desc_t { app_input_cluster_count,
app_output_cluster_count, ... }`; ZDO — `zdo_dev_srv_disc.h`:
`Simple_Desc_req` (endpoint → descriptor), `Active_EP_req` (список endpoint'ов).

Маппинг input/output → роль задан спецификацией (ZDO Match Descriptor,
`zdo_dev_srv_disc.h:191-196`):

```text
input  clusters = server  (0x01)
output clusters = client  (0x02)
```

Роли: `EZB_ZCL_CLUSTER_SERVER = 0x01`, `EZB_ZCL_CLUSTER_CLIENT = 0x02`
(`ezbee/zcl/zcl_type.h`).

## 2. Роль кластера решает всё

| Сторона | Что означает | Атрибуты (состояние) | Команды |
|---|---|---|---|
| **server** | endpoint реализует функциональность | **держит** атрибуты, репортит | **принимает** |
| **client** | endpoint использует функциональность | своего состояния нет | **отправляет** |

Правило: **состояние существует только на server-стороне**. Client — источник
команд/событий, а не состояние.

Это относится к кластеру **на конкретном endpoint**, а не к устройству целиком:
одно устройство может быть server'ом для одного кластера и client'ом для другого
(и на разных endpoint'ах).

## 3. Один кластер — противоположные роли

`OnOff` (cluster_id `0x0006`), самый наглядный случай:

| Роль | Кто | Что делает | У нас |
|---|---|---|---|
| **server** | реле / лампа / розетка | держит `OnOff`, принимает `Off/On/Toggle`, репортит | `HA_ENTITY_STATE` на атрибут + capability `switch`; цель команд |
| **client** | кнопка / пульт / выключатель | отправляет `Off/On/Toggle` | состояния нет; нажатие — **событие** (`EVENT`) |

То же для `Level Control` (`0x0008`): server — диммируемая лампа, client —
диммер-контроллер (крутилка/кнопки).

`device_id` — **уточнение ярлыка, а не основа** классификации. Функционально
решают `(cluster_id, role)`.

## 4. Проекция: `(cluster_id, role) → capability`

Это **вычисляемая проекция**, а не хранимая сущность (разделение —
`WEB.md §2`). Domain хранит сырой endpoint; capability строит потребитель.

| cluster_id | имя | role | capability | что даёт |
|---|---|---|---|---|
| `0x0006` | OnOff | server | `switch` | управляемая нагрузка, состояние on/off |
| `0x0008` | Level Control | server | `dimmable` | уровень 0..254 (включает `switch`) |
| `0x0300` | Color Control | server | `color` | цвет/цветовая температура (включает `dimmable`) |
| `0x0100` | Shade Config | server | `cover` | привода/шторы |
| `0x0102` | Window Covering | server | `cover` | то же, новее |
| `0x0201` | Thermostat | server | `thermostat` | уставки/режимы HVAC |
| `0x0202` | Fan Control | server | `fan` | вентилятор |
| `0x0400` | Illuminance Meas. | server | `sensor.illuminance` | освещённость |
| `0x0402` | Temperature Meas. | server | `sensor.temperature` | температура |
| `0x0405` | Rel. Humidity Meas. | server | `sensor.humidity` | влажность |
| `0x0406` | Occupancy Sensing | server | `sensor.occupancy` | присутствие |
| `0x0500` | IAS Zone | server | `alarm.zone` | охранный шлейф |
| `0x0702` | Metering | server | `metering` | показания счётчика |
| `0x0B04` | Electrical Meas. | server | `metering` | ток/напряжение/мощность |
| `0x0006` | OnOff | **client** | `controller.button` | источник команд (кнопка/пульт) |
| `0x0008` | Level Control | **client** | `controller.dimmer` | источник команд уровня |
| `0x0005` | Scenes | **client** | `controller.scene` | сцена-контроллер |

Дополнительно `OnOffSwitchConfig` (`0x0007`, server) уточняет характер кнопки:
`SwitchType` = `Toggle` / `Momentary` / `Multifunction` (§6).

Проекцию строит **потребитель из кластеров endpoint'а**, хранимых сырыми. Capability
**не хранится** в Domain и не держится словарём в прошивке. На практике проекцию
выполняет клиент — `web-ui/src/capabilities.js` (`deriveEndpointMeta`): сопоставляет
`(cluster_id, role)` тегам `accepts/emits/reports`, по ним выбираются виджеты. Таблица
§4 — спецификация этой проекции (серверный словарь `ha_capabilities.h` был мёртвым и
удалён).

## 5. Примеры устройств (деревом)

> Состав — **типовой** для типа устройства. Точный состав всегда определяет
> Simple Descriptor самого устройства, который читается на интервью.

### 5.1. Dimmable Light (`device_id 0x0101`)

Реле-лампа: всё **server**, состояние on/off и уровень.

```text
device 00124B000A1B2C3D          device_id 0x0101  Dimmable Light
└── endpoint 1
    ├── Basic           0x0000  server   model / manufacturer
    ├── Identify        0x0003  server
    ├── Groups          0x0004  server
    ├── Scenes          0x0005  server
    ├── OnOff           0x0006  server   ← реле: состояние on/off, принимает Off/On/Toggle
    └── Level Control   0x0008  server   ← диммер: CurrentLevel 0..254
```

Проекция: `switch` + `dimmable`.

### 5.2. On/Off Switch / remote (`device_id 0x0000`)

Кнопка/пульт: `OnOff` — **client**, своего состояния нет.

```text
device 00124B000A1B2C3D          device_id 0x0000  On/Off Switch
└── endpoint 1
    ├── Basic           0x0000  server
    ├── Identify        0x0003  server
    └── OnOff           0x0006  client   ← кнопка: отправляет Off/On/Toggle
```

Проекция: `controller.button`. Нажатие — событие; чтобы его увидеть, client-кластер
кнопки должен быть сбайнжен на координатор (иначе команда уходит напрямую цели).

### 5.3. Dimmer Switch (`device_id 0x0104`)

Контроллер уровня: `OnOff` + `Level` — **client**.

```text
device ...                       device_id 0x0104  Dimmer Switch
└── endpoint 1
    ├── Basic           0x0000  server
    ├── Identify        0x0003  server
    ├── OnOff           0x0006  client
    └── Level Control   0x0008  client   ← крутилка/кнопки вверх-вниз
```

Проекция: `controller.dimmer`.

### 5.4. Temperature Sensor (`device_id 0x0302`)

Датчик: единственный измерительный кластер — server.

```text
device ...                       device_id 0x0302  Temperature Sensor
└── endpoint 1
    ├── Basic                   0x0000  server
    ├── Identify                0x0003  server
    └── Temperature Meas.       0x0402  server   ← MeasuredValue (0.01 °C)
```

Проекция: `sensor.temperature`. Состояние — по атрибуту `MeasuredValue`.

### 5.5. Occupancy Sensor (`device_id 0x0107`)

```text
device ...                       device_id 0x0107  Occupancy Sensor
└── endpoint 1
    ├── Basic                   0x0000  server
    ├── Identify                0x0003  server
    └── Occupancy Sensing       0x0406  server   ← Occupancy (bitmap)
```

Проекция: `sensor.occupancy`.

### 5.6. Two-gang relay (тип часто vendor-specific; близко к `0x0002`)

Два независимых реле — **два endpoint'а**, каждый server-OnOff.

```text
device ...                       device_id 0x0002 (On/Off Output) / vendor
├── endpoint 1
│   ├── OnOff         0x0006  server   ← реле 1
│   └── OnOffSwitchCfg 0x0007 server
└── endpoint 2
    ├── OnOff         0x0006  server   ← реле 2
    └── OnOffSwitchCfg 0x0007 server
```

Проекция — по каждому endpoint'у отдельно; устройство = объединение возможностей.

### 5.7. Wall switch с реле и кнопкой (vendor-specific)

Один физический выключатель: кнопка **и** реле. Кнопка — `client`, реле — `server`.

```text
device ...                       vendor-specific
├── endpoint 1                        реле (нагрузка)
│   ├── OnOff         0x0006  server  ← управляемая нагрузка
│   └── OnOffSwitchCfg 0x0007 server
└── endpoint 2                        кнопка (вход)
    └── OnOff         0x0006  client  ← щелчок = событие
```

Здесь один `device` даёт и capability `switch` (EP1), и `controller.button` (EP2).

## 6. Устойчивое vs моментное (event vs state)

Server-атрибут может быть устойчивым или моментным:

- **устойчивое** (OnOff `OnOff`, Level `CurrentLevel`, MeasuredValue) →
  `HA_ENTITY_STATE`, `changed` имеет смысл;
- **моментное** (нажатие, тревога, импульс) → `EVENT` / `domain_payload_put`,
  состояния не создаёт.

Различители из спецификации:

- `OnOffSwitchConfig` (`0x0007`) `SwitchType`: `Toggle` (0x00) / `Momentary`
  (0x01) / `Multifunction` (0x02); `SwitchActions`: `On`/`Off`/`Toggle`
  (`ezbee/zcl/cluster/on_off_switch_config_desc.h`);
- `Multistate Input` (`0x0012`) — `PresentValue` как код состояния кнопки/переключателя;
- `IAS Zone` (`0x0500`) — тревога/статус шлейфа.

Именно сервис решает, что считать state, а что event; классификация — не
автоматическая по кластеру, а решение модели Zigbee-слоя.

## 7. Что хранит наш слой, что проецируется

```text
Храним (canonical, Domain):
    ha_endpoint_record_t { profile_id, device_id, cluster_count, clusters[(cluster_id, role)] }
    ha_zb_state_record_t { raw, zcl_type }  на атрибут
    ha_device_record_t   { name, model }    из Basic (ModelIdentifier, ManufacturerName)

Не храним (проекция потребителя):
    capability (switch / dimmable / sensor.* / controller.*)
```

Ключевые атрибуты Basic для ярлыка (`ezbee/zcl/cluster/basic_desc.h`):
`ZCLVersion 0x0000`, `ManufacturerName 0x0004`, `ModelIdentifier 0x0005`,
`PowerSource 0x0007`.

## 8. Источники и проверка

Референсы спецификаций:

- **ZCL** — Zigbee Cluster Library Specification, документ `07-5123`
  (наш словарь сверялся по ZCL6, `07-5123-06`, `ha_zigbee.h:9`);
- **ZHA** — Zigbee Home Automation Specification, Revision 29, Version 1.2
  (источник `device_id`, `zha.h:15`);
- **Zigbee Specification** — ZDO Device/Service Discovery: `Simple_Desc_req`,
  `Active_EP_req`, `Match_Desc_req` (input=server / output=client).

Сверено по заголовкам стека `esp-zigbee-lib` (реализация спецификации):

```text
ezbee/zha.h                     device_id (0x0000..), комментарии по типам
ezbee/zcl/zcl_type.h            cluster_id (OnOff 0x0006, Level 0x0008, ...),
                                foundation-команды (Read 0x00, ConfigureReporting 0x06,
                                ReportAttributes 0x0a, DiscoverAttributes 0x0c),
                                роли EZB_ZCL_CLUSTER_SERVER=0x01 / CLIENT=0x02
ezbee/af.h                      ezb_af_simple_desc_t (input/output cluster lists)
ezbee/zdo/zdo_dev_srv_disc.h    Simple_Desc_req, Active_EP_req, Match_Desc_req
                                (in=server / out=client), Device_Annce
ezbee/zcl/cluster/on_off_switch_config_desc.h   SwitchType / SwitchActions
ezbee/zcl/cluster/multistate_input.h            Multistate Input
ezbee/zcl/cluster/basic_desc.h                  Basic attr IDs
```

## 9. Открытые вопросы

1. ~~**Словарь в `ha_model`.**~~ Отменено: серверный словарь был мёртвым и удалён;
   проекция — на клиенте (`web-ui/src/capabilities.js`), таблица §4 — её спецификация.
2. **Гранулярность до атрибута.** Endpoint хранит только кластер + роль; для
   некоторых возможностей нужно знать атрибуты (например, `ColorCapabilities`).
3. **Кнопки.** Политика байндинга client-кластеров кнопки на координатор,
   чтобы нажатия были видны; классификация моментного как `EVENT`.
4. **Vendor-specific кластеры/device_id** (многоклавишные выключатели, реле с
   кнопками): как проецировать неизвестные комбинации.
5. **`Multistate Input` ApplicationType** — таблица кодов кнопок/HVAC.
