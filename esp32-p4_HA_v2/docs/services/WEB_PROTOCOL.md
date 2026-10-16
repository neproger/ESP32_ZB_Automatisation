# Web protocol (бинарный, canonical verbatim, v3)

> Контракт провода между браузером и `web`-сервисом. Роль сервиса — `WEB.md`,
> разводка Wi-Fi — `../hardware/JC4880P443C_I_W.md` §4.5. Реализация — `web/src`,
> браузерная сторона — `web-ui/src/proto.js`.

## 1. Принципы

- Транспорт — бинарный WebSocket (`/ws`), кадры `HTTPD_WS_TYPE_BINARY`. Маленький endian.
- **`web` — тупой транспорт canonical-модели**: он не интерпретирует Domain. Каждый
  кадр несёт canonical `key`/`record` **verbatim** (layout = `ha_model`); разбор и
  семантика — в браузере. Компактных DTO/кодеков в `web` нет.
- `type` сущностных кадров = числовое значение `ha_entity_t` (стабильный canonical ABI;
  значения не переиспользуются). Управляющие кадры выбраны вне диапазона `ha_entity_t`.
- Строк в протоколе нет; строки — только пользовательские имена внутри canonical записи.
- Заголовок 8 байт; длина — из заголовка. Все canonical формы — фиксированного размера
  (`static_assert` в `ha_model`), переменную длину payload не несёт.

## 2. Заголовок (8 байт)

| offset | тип | поле |
|---|---|---|
| 0 | u8 | `ver` = 3 |
| 1 | u8 | `type` (см. §3) |
| 2 | u16 | `len` — длина payload |
| 4 | u16 | `seq` — корреляция (транспорт) |
| 6 | u16 | `flags` — зарезервировано (0) |

### 2.1. Транспорт: per-client TX queue, back-to-back

Транспорт тупой: знает только байты и `seq`, не различает типы сообщений. Порядок и
надёжность даёт TCP, поэтому пофреймовый ACK/retry не нужен.

```text
web_sessions_send/broadcast(fd, ...) — поставить кадр в очередь клиента (никогда не шлёт);
                                       seq присваивает транспорт
web_sessions_step()                  — web-задача шлёт накопленное бэк-ту-бэк
```

- каждый кадр device → browser несёт `header.seq` (per-client, с 0);
- очередь bounded (64 кадра / 16 КБ): клиент не успевает → сессия закрывается
  (`httpd_sess_trigger_close`); после reconnect клиент получает свежий snapshot;
- порядок железный (TCP): `send A,B,C` → клиент видит A→B→C;
- браузер дедуплицирует по `seq` (повторно не применяет);
- отправка идёт из web-задачи и **никогда — под Domain-lock**.

Контракт: *within an active session — ordered (TCP); across disconnect — no guarantee;
reconnect + snapshot восстанавливают истину.*

## 3. Типы кадров

### 3.1. Управляющие

| type | имя | payload |
|---|---|---|
| 0x01 | SYNC_BEGIN | `u32 count` |
| 0x02 | SYNC_END | `u32 count` |
| 0xE0 | EVENT | `ha_event_t` verbatim |
| 0x20 | COMMAND | `u8 cmd` + args (browser → device; см. §6) |

### 3.2. Сущностные (`type = ha_entity_t`)

Один формат на все сущности: **UPSERT** = `[key][record]`, **REMOVE** = `[key]`
(`key`/`record` — canonical формы, §4). Snapshot использует только UPSERT.

| type | сущность | key | record |
|---|---|---|---|
| 4 | AUTOMATION | `ha_automation_key_t` (8) | `ha_rule_t` (392) |
| 6 | LOCATION | `ha_device_id_t` (8) | `ha_location_record_t` (60) |
| 7 | GROUP | `ha_group_key_t` (8) | `ha_group_record_t` (32) |
| 8 | GROUP_ITEM | `ha_group_item_key_t` (16) | `ha_group_item_record_t` (36) |
| 9 | WEATHER | `ha_device_id_t` (8) | `ha_weather_record_t` (16) |
| 12 | WIFI_STATUS | `ha_device_id_t` (8) | `ha_wifi_status_record_t` (36) |
| 13 | SETTINGS | `ha_settings_key_t` (1) | `ha_settings_record_t` (8) |
| 14 | ENTITY | `ha_entity_key_t` (8) | `ha_entity_record_t` (40) |
| 15 | PROPERTY_STATE | `ha_state_key_t` (16) | `ha_state_record_t` (8) |
| 16 | CAPABILITIES | `ha_entity_key_t` (8) | `ha_entity_capabilities_record_t` (216) |
| 17 | LOGICAL_DEVICE | `ha_logical_device_key_t` (8) | `ha_logical_device_record_t` (80) |

Список типов в браузер — **явный** (`SCHEMA` в `web/src/web.c`): новая внутренняя
Domain-таблица не становится публичным API автоматически. `wifi_scan`/`wifi_known`
в браузер не отдаются (велики/пароли) — их читает Display из Domain.

## 4. Формы canonical записей (little-endian, verbatim `ha_model`)

| сущность | структура и layout |
|---|---|
| Entity | `ha_entity_record_t` (40): `device_id u64@0`, `name[32]@8` |
| Logical Device | `ha_logical_device_record_t` (80): `parent_device_id u64@0`, `source u32@8`, `native_id[32]@12`, `name[32]@44`, `reserved[4]@76` |
| Property state | `ha_state_key_t` (16): `entity u64@0`, `property i32@8`, `reserved u32@12`; record `ha_value_t` (8): `kind i32@0`, `value u32/f32@4` |
| Capabilities | `ha_entity_capabilities_record_t` (216): `property_count u8@0`, `reserved[3]`, `properties[8]` (`ha_capability_t` 24: `property i32@0`, `action_count u8@4`, `actions[4] i32@8`), `event_count u8@196`, `events[4] i32@200` |
| Automation | `ha_rule_t` (392) — см. §5 |
| Group / item | `ha_group_record_t` (32): `title[32]`; `ha_group_item_key_t` (16): `group_id u64@0`, `entity u64@8` (плитка = Entity); `ha_group_item_record_t` (36): `order u16@0`, `reserved[2]@2`, `title[32]@4` |
| Location | `ha_location_record_t` (60): `latitude f32@0`, `longitude f32@4`, `tz_offset_min i16@8`, `flags u8@10`, `reserved u8@11`, `name[48]@12` |
| Weather | `ha_weather_record_t` (16): `condition u8@0`, `cloud_pct u8@1`, `temperature_c100 i16@2`, `humidity_p100 u16@4`, `pressure_hpa u16@6`, `wind_kmh10 u16@8`, `wind_dir_deg u16@10`, `reserved[4]@12` |
| Wi-Fi status | `ha_wifi_status_record_t` (36): `state u8@0`, `connected u8@1`, `rssi i8@2`, `reserved u8@3`, `ssid[32]@4` |
| Settings | `ha_settings_record_t` (8): `screensaver_timeout_ms u32@0`, `brightness_pct u8@4`, `palette_id u8@5`, `reserved[2]@6` |

## 5. Rule wire = `ha_rule_t` (392, плоский POD)

Значимость полей — по `kind` (`ha_automation_trigger_kind_t`: 0 EVENT, 1 TIME, 2 STATE);
незначимые поля — ноль. Инварианты `offsetof` — `ha_model/ha_rule.h`. **Любой** из
`trigger_count` триггеров запускает **все** `action_count` действий; условия — общие.

```text
@0    triggers[4] { entity u64@0, event_id i32@8, property i32@12, value f32@16,
                    value2 f32@20, minutes_of_day u16@24, kind u8@26, op u8@27,
                    edge u8@28, weekday_mask u8@29, reserved[2]@30 }        (32 each)
@128  actions[4]  { ref(16)@0 {entity u64, property i32@8, reserved u32@12},
                    value(12)@16 { kind i32, scalar{ kind i32@+4, value u32/f32@+8 } |
                    xy{ x f32@+4, y f32@+8 } }, action u8@28, reserved[3]@29 } (32 each)
@256  conditions[4] { ref(16), cmp{ op u8@16, reserved[3], value f32@20, value2 f32@24 },
                      reserved u32@28 }                                     (32 each)
@384  enabled u8; @385 trigger_count u8; @386 action_count u8; @387 conditions_count u8;
      @388 reserved[4] (явный tail padding)
```

`entity == 0xFFFFFFFFFFFFFFFF` (`HA_ENTITY_ID_SOURCE`) — «то же, что источник триггера».

## 6. Команды (browser → device) — args verbatim

Args каждой команды = `sizeof` соответствующей canonical-формы (включая явный tail padding;
см. `reserved2` у `ha_property_command_t`). Сервер проверяет `len == sizeof`, поэтому скрытый
padding недопустим.

| cmd | имя | args (canonical) |
|---|---|---|
| 1 | SNAPSHOT | — (повторная высылка этому клиенту) |
| 4 | AUTOMATION_PUT | `ha_automation_key_t` + `ha_rule_t` |
| 5 | AUTOMATION_REMOVE | `ha_automation_key_t` |
| 8 | DISCOVERY | `u8 seconds` (0 — stop). Adapter решает, что это (Zigbee → permit join) |
| 9 | GROUP_PUT | `ha_group_key_t` + `ha_group_record_t` |
| 10 | GROUP_REMOVE | `ha_group_key_t` |
| 13 | LOCATION_PUT | `ha_device_id_t` + `ha_location_record_t` |
| 16 | GROUP_ITEM_PUT | `ha_group_item_key_t` + `ha_group_item_record_t` |
| 17 | GROUP_ITEM_REMOVE | `ha_group_item_key_t` |
| 18 | ENTITY_RENAME | `ha_entity_rename_t` (40): `entity u64@0`, `name[32]@8` |
| 19 | PROPERTY_COMMAND | `ha_property_command_t` (32): `entity u64@0`, `property i32@8`, `action u8@12`, `reserved[3]`, `ha_command_value_t@16` (`kind i32`, `scalar{kind i32, value}` / `xy{x,y}`), `reserved2[4]@28`. Wire = `sizeof` (включая явный tail padding) |
| 20 | DEVICE_RENAME | `ha_device_id_t` + `char name[32]` |
| 21 | LOGICAL_DEVICE_REMOVE | `ha_device_id_t` |

## 7. Сценарии

```text
WS connect (GET): сервер принимает соединение. Snapshot шлёт сам клиент командой SNAPSHOT
  сразу после open (в GET слать нельзя: 101 завершается после возврата хендлера).

client → SNAPSHOT:
  SYNC_BEGIN{count} → UPSERT(type=ha_entity_t){key,record} × N → SYNC_END{count}

Domain subscription (любые source; фильтр — по типу факта):
  ENTITY_UPSERTED → read record → UPSERT(key,record) → broadcast
  ENTITY_REMOVED  → REMOVE(key) → broadcast
  EVENT           → payload ha_event_t → broadcast(EVENT)

WS COMMAND (args verbatim):
  PROPERTY_COMMAND       → ha_property_command_t → domain_post(HA_CMD_PROPERTY)
  ENTITY_RENAME          → domain_post(HA_CMD_ENTITY_RENAME)
  DEVICE_RENAME          → device_rename (Device service; имя принадлежит Logical Device)
  AUTOMATION_PUT/REMOVE  → domain_entity_put/remove(AUTOMATION)
  LOGICAL_DEVICE_REMOVE  → device_remove (Device service → remover → cascade)
  DISCOVERY              → domain_post(HA_CMD_DISCOVERY)
  GROUP_*/LOCATION_PUT/GROUP_ITEM_* → domain_entity_put/remove
```

## 8. Решённые вопросы

- ~~snapshot под Domain-lock~~ — решено: snapshot и дельты только кладут кадры в очередь
  (`web_sessions_send`); отправляет `web_sessions_step` из web-задачи (не под Domain-lock).
- ~~медленный клиент~~ — решено: bounded-очередь, отправка бэк-ту-бэк; не успевает →
  сессия закрывается, reconnect даёт свежий snapshot.
- ~~компактные DTO и кодеки в web~~ — решено (Step R): canonical layout = wire layout;
  web несёт canonical structs verbatim, интерпретация — в браузере. Старые протоколы
  (v2, DTO-раскладка v3) не поддерживаются.
