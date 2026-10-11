# Web protocol (бинарный, canonical v3)

> Контракт провода между браузером и `web`-сервисом. Роль сервиса — `WEB.md`,
> разводка Wi-Fi — `../hardware/JC4880P443C_I_W.md` §4.5. Реализация — `web/src`,
> браузерная сторона — `web-ui/src/proto.js`.

## 1. Принципы

- Транспорт — бинарный WebSocket (`/ws`), кадры `HTTPD_WS_TYPE_BINARY`. Маленький endian.
- Payload — **entity-native canonical DTO**: identity это `entity_id` / `device_id` /
  `(parent)`, без числовых Domain-типов и без `uid/endpoint/cluster/attr/zcl_type`.
- Строк в протоколе нет; строки — только пользовательские имена внутри DTO.
- Заголовок 8 байт; длина — из заголовка. Размеры DTO фиксированы (см. §4), переменную
  длину несёт только `AUTOMATION`/`CAPABILITIES`.

## 2. Заголовок (8 байт)

| offset | тип | поле |
|---|---|---|
| 0 | u8 | `ver` = 2 (версия кадра; набор типов — canonical) |
| 1 | u8 | `type` (см. §3) |
| 2 | u16 | `len` — длина payload |
| 4 | u16 | `seq` — корреляция команд |
| 6 | u16 | `flags` — зарезервировано (0) |

## 3. Типы сообщений (device → browser)

| type | имя | payload |
|---|---|---|
| 0x01 | SYNC_BEGIN | `u32 count` |
| 0x02 | SYNC_END | `u32 count` |
| 0x30 | ENTITY | `u64 entity_id, u64 device_id, char name[32]` |
| 0x31 | ENTITY_REMOVE | `u64 entity_id` |
| 0x32 | PROPERTY_STATE | `u64 entity_id, u16 property, u8 kind, u32 bits` |
| 0x33 | PROPERTY_STATE_REMOVE | `u64 entity_id, u16 property` |
| 0x34 | CAPABILITIES | `u64 entity_id, u8 count, props[]{u16 property,u8 n,u8 actions[]}, u8 event_count, u8 events[]` |
| 0x35 | CAPABILITIES_REMOVE | `u64 entity_id` |
| 0x36 | AUTOMATION | `u64 id | ha_rule_t (entity-native)` |
| 0x37 | AUTOMATION_REMOVE | `u64 id` |
| 0x38 | EVENT | `u64 source_entity, u8 event_id, u8 value_kind, u32 value_bits` |
| 0x39 | DEVICE | `u64 device_id, u64 parent_device_id, char name[32]` |
| 0x3A | DEVICE_REMOVE | `u64 device_id` |
| 0x19 | GROUP_ITEM | `u64 group_id, u64 entity, u16 property, u16 order, char title[32]` |
| 0x1A | GROUP_ITEM_REMOVE | `u64 group_id, u64 entity, u16 property` |
| 0x3B | LOCATION | `u64 device_id | ha_location_record_t` |
| 0x3C | LOCATION_REMOVE | `u64 device_id` |
| 0x3D | WEATHER | `u64 device_id | ha_weather_record_t` |
| 0x3E | WEATHER_REMOVE | `u64 device_id` |
| 0x3F | WIFI_STATUS | `u64 device_id | ha_wifi_status_record_t` |
| 0x40 | WIFI_STATUS_REMOVE | `u64 device_id` |
| 0x41 | SETTINGS | `u8 id | ha_settings_record_t` |
| 0x42 | SETTINGS_REMOVE | `u8 id` |
| 0x43 | GROUP | `u64 id | ha_group_record_t` |
| 0x44 | GROUP_REMOVE | `u64 id` |

`SYNC_BEGIN/END` обрамляют snapshot. Удаление — соответствующий `*_REMOVE` (key-only).
`wifi_scan`/`wifi_known` в браузер не отдаются (велик/пароли) — их читает Display из Domain.

## 4. Формы canonical записей

Источник истины — `ha_model`. Байтовые form'ы (little-endian, фиксированный размер):

| сущность | форма |
|---|---|
| Entity | `ha_entity_record_t` (device_id + name) |
| Logical Device | `ha_logical_device_record_t` (parent_device_id, source, native_id, name) |
| Property state | `ha_state_record_t` (`ha_value_t`: kind + bits) |
| Capabilities | `ha_entity_capabilities_record_t` (properties[] + events[]) |
| Automation | `ha_rule_t` (entity-native; см. §5) |
| Location | `ha_location_record_t` (60) |
| Weather | `ha_weather_record_t` (16) |
| Wi-Fi status | `ha_wifi_status_record_t` (36) |
| Settings | `ha_settings_record_t` (8) |
| Group / group_item | `ha_group_record_t` (32) / `ha_group_item_record_t` (36) |

## 5. Rule wire (entity-native)

`AUTOMATION` и `WEB_CMD_AUTOMATION_PUT` совпадают:

```text
u64 id; u8 enabled; u8 kind;
trigger{ EVENT: u64 entity + u8 event_id
       | TIME:  u16 minutes_of_day + u8 weekday_mask
       | STATE: u64 entity + u16 property + u8 op + u8 edge + f32 value + f32 value2 }
u8 conditions_count; conditions[]{ u64 entity, u16 property, u8 op, f32 value, f32 value2 }
action{ u64 entity, u16 property, u8 action, u8 value_kind
        [, SCALAR: u8 kind + u32/f32 | XY: f32 x + f32 y ] }
```

`entity == 0xFFFFFFFFFFFFFFFF` (`HA_ENTITY_ID_SOURCE`) — «то же, что источник триггера».

## 6. Команды (browser → device)

| cmd | имя | args |
|---|---|---|
| 1 | SNAPSHOT | — (повторная высылка этому клиенту) |
| 4 | AUTOMATION_PUT | `u64 id | rule` (§5) |
| 5 | AUTOMATION_REMOVE | `u64 id` |
| 8 | DISCOVERY | `u8 seconds` (0 — stop). Adapter решает, что это (Zigbee → permit join) |
| 9 | GROUP_PUT | `u64 id | char title[32]` |
| 10 | GROUP_REMOVE | `u64 id` |
| 13 | LOCATION_PUT | `u64 device_id | ha_location_record_t` |
| 16 | GROUP_ITEM_PUT | `u64 group_id, u64 entity, u16 property, u16 order, char title[32]` |
| 17 | GROUP_ITEM_REMOVE | `u64 group_id, u64 entity, u16 property` |
| 18 | ENTITY_RENAME | `u64 entity_id | char name[32]` |
| 19 | PROPERTY_COMMAND | `u64 entity_id, u16 property, u8 action, u8 value_kind[, scalar | xy]` |
| 20 | DEVICE_RENAME | `u64 device_id | char name[32]` |
| 21 | LOGICAL_DEVICE_REMOVE | `u64 device_id` |

## 7. Сценарии

```text
WS connect (GET): сервер принимает соединение. Snapshot шлёт сам клиент командой SNAPSHOT
  сразу после open (в GET слать нельзя: 101 завершается после возврата хендлера).

client → SNAPSHOT:
  SYNC_BEGIN{count} → ENTITY/DEVICE/PROPERTY_STATE/CAPABILITIES/AUTOMATION/GROUP_ITEM
  + LOCATION/WEATHER/WIFI_STATUS/SETTINGS/GROUP → SYNC_END{count}

Domain subscription (любые source; фильтр — по типу факта):
  UPSERTED → canonical DTO → broadcast; REMOVED → *_REMOVE → broadcast

WS COMMAND:
  PROPERTY_COMMAND       → ha_property_command_t → domain_post(HA_CMD_PROPERTY)
  ENTITY_RENAME/DEVICE_RENAME → domain_entity_put
  AUTOMATION_*           → domain_entity_put/remove(AUTOMATION)
  LOGICAL_DEVICE_REMOVE  → device_remove (Device service → remover → cascade)
  DISCOVERY              → domain_post(HA_CMD_DISCOVERY)
  GROUP_*/LOCATION_PUT/GROUP_ITEM_* → domain_entity_put/remove
```

## 8. Открытые вопросы

- snapshot шлётся под Domain-lock (итерация + отправка); при медленном клиенте это
  задерживает писателей Domain — при росте числа клиентов собирать в буфер и слать после
  освобождения lock.
