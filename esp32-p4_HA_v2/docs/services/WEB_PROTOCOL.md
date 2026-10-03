# Web protocol v2 (бинарный, raw entities)

> Контракт провода между браузером и `web`-сервисом. Роль сервиса — `WEB.md`,
> разводка Wi-Fi — `../hardware/JC4880P443C_I_W.md` §4.5. Реализация — `web/src`.

## 1. Принципы

- Транспорт — бинарный WebSocket (`/ws`), кадры `HTTPD_WS_TYPE_BINARY`.
- Маленький endian (`LE`).
- Payload — **сырые записи Domain как есть** (`ha_model/ha_entities.h`), без проекции
  на стороне прошивки и без копирования в DTO. Браузер декодирует по схеме (§5).
- Строк в протоколе нет. Единственные строки — пользовательские `device.name`/`model`
  внутри самой записи.
- Заголовок 8 байт. Длина — из заголовка; размеры ключа/записи браузер знает из схемы.

## 2. Заголовок (8 байт)

| offset | тип | поле |
|---|---|---|
| 0 | u8 | `ver` = 2 |
| 1 | u8 | `type` (см. §3) |
| 2 | u16 | `len` — длина payload |
| 4 | u16 | `seq` — корреляция команд/ответов |
| 6 | u16 | `flags` — зарезервировано (0) |

## 3. Типы сообщений

| type | имя | payload |
|---|---|---|
| 0x01 | SYNC_BEGIN | `u32 count` |
| 0x02 | SYNC_END | `u32 count` |
| 0x10 | ENTITY | `u8 type | key | record` |
| 0x11 | ENTITY_REMOVE | `u8 type | key` |
| 0x20 | COMMAND | `u8 cmd | args` |
| 0x21 | CMD_RESULT | `i16 status` |

`ENTITY`/`ENTITY_REMOVE` — и в snapshot, и в дельте. `key`/`record` — сырые байты
layout'а типа (§5). `SYNC_BEGIN/END` обрамляют snapshot.

## 4. Команды (browser → device)

| cmd | имя | args |
|---|---|---|
| 1 | SNAPSHOT | — (повторная высылка этому клиенту) |
| 2 | ZB_COMMAND | `ha_zb_command_t` |
| 3 | DEVICE_RENAME | `u64 uid | char name[32]` |
| 4 | AUTOMATION_PUT | `u64 id | ha_automation_record_t` |
| 5 | AUTOMATION_REMOVE | `u64 id` |

Ответ — `CMD_RESULT` с тем же `seq`; `status` = 0 (OK) или `sys_error` code.

## 5. Схема записей (key/record)

Дискриминатор типа совпадает с `ha_entity_t` (`ha_model/ha_entities.h`):

| type | сущность | key | record |
|---|---|---|---|
| 1 | device | `u64 uid` | `{ char name[32]; char model[32]; }` |
| 2 | state | `{ u64 uid; u16 cluster; u16 attr; u8 ep; u8 rsv[3]; }` (16) | `{ u32 raw; u8 zcl_type; u8 rsv[3]; }` (8) |
| 3 | endpoint | `{ u64 uid; u8 ep; u8 rsv[7]; }` (16) | `{ u16 profile; u16 device_id; u8 count; u8 rsv[3]; cluster[16]{u16 id; u8 role; u8 rsv;} }` |
| 4 | automation | `u64 id` | `{ u8 enabled; u8 args_len; u8 rsv[6]; u64 trigger_uid; u16 trigger_cmd; u64 action_uid; u8 action_ep; u16 action_cluster; u8 action_cmd; u8 action_args[8]; }` |

Заголовки/структуры — источник истины `ha_model`. Браузерная копия схемы —
`web-ui/src/schema.js`.

## 6. Сценарии

```text
WS connect (GET):
  web task → этому fd: SYNC_BEGIN{count}, ENTITY per record (по типам), SYNC_END{count}

Domain subscription (ENTITY_UPSERTED | ENTITY_REMOVED, любые source/entity):
  UPSERTED → domain_entity_get → ENTITY → broadcast всем WS-клиентам
  REMOVED  → ENTITY_REMOVE → broadcast

WS COMMAND:
  ZB_COMMAND      → domain_post(HA_CMD_ZIGBEE_CLUSTER, …)
  DEVICE_RENAME   → domain_entity_get/put(device)
  AUTOMATION_*    → domain_entity_put/remove(automation)
  SNAPSHOT        → повторный snapshot этому клиенту
  → CMD_RESULT{seq,status}
```

## 7. Открытые вопросы

- snapshot шлётся под Domain-lock (итерация + отправка). При медленном клиенте это
  задерживает писателей Domain; при росте числа клиентов — собирать в буфер и слать
  после освобождения lock.
- события (`EVENT`, type 0x30) пока не шлются: добавим вместе с потребителем в UI.
