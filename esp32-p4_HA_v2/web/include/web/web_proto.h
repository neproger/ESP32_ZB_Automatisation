#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Web transport protocol v2 (docs/services/WEB_PROTOCOL.md): binary, little-endian,
 * сырые записи Domain в payload. Тут — только раскладка кадра и дискриминаторы;
 * семантику и схемы записей знает вызывающий (web.c) и браузер.
 */

#define WEB_PROTO_VER 2u
#define WEB_PROTO_HDR_SIZE 8u

/* Максимальный payload одного кадра: u8 type + самый большой key+record. */
#define WEB_PROTO_MAX_PAYLOAD 256u
#define WEB_PROTO_MAX_FRAME (WEB_PROTO_HDR_SIZE + WEB_PROTO_MAX_PAYLOAD)

/* Совпадает с ha_entity_t (ha_model/ha_entities.h); проверяется static_assert в web.c. */
#define WEB_ENTITY_DEVICE 1u
#define WEB_ENTITY_STATE 2u
#define WEB_ENTITY_ENDPOINT 3u
#define WEB_ENTITY_AUTOMATION 4u
#define WEB_ENTITY_DEVICE_REMOVE 5u
#define WEB_ENTITY_LOCATION 6u
#define WEB_ENTITY_GROUP 7u
#define WEB_ENTITY_GROUP_ITEM 8u
#define WEB_ENTITY_WEATHER 9u
/* wifi_scan (10) и wifi_known (11) в браузер не отдаются: первая слишком велика
 * (16 AP), вторая содержит пароли. Их читает Display напрямую из Domain. */
#define WEB_ENTITY_WIFI_STATUS 12u
#define WEB_ENTITY_SETTINGS 13u

typedef enum {
    WEB_MSG_SYNC_BEGIN = 0x01,
    WEB_MSG_SYNC_END = 0x02,
    WEB_MSG_ENTITY = 0x10,
    WEB_MSG_ENTITY_REMOVE = 0x11,
    /*
     * Аддитивно (Фаза 5.1): семантическое состояние рядом с raw ENTITY STATE.
     * STATE DTO (16): u64 uid @0, u8 ep @8, u16 property @9, u8 kind @11, u32 bits @12.
     * REMOVE (11): u64 uid @0, u8 ep @8, u16 property @9. Без cluster/attr/zcl_type/raw.
     */
    WEB_MSG_SEMANTIC_STATE = 0x12,
    WEB_MSG_SEMANTIC_STATE_REMOVE = 0x13,
    WEB_MSG_COMMAND = 0x20,
} web_msg_t;

typedef enum {
    WEB_CMD_SNAPSHOT = 1,
    WEB_CMD_ZB_COMMAND = 2,
    WEB_CMD_DEVICE_RENAME = 3,
    WEB_CMD_AUTOMATION_PUT = 4,
    WEB_CMD_AUTOMATION_REMOVE = 5,
    WEB_CMD_DEVICE_REMOVE = 6,
    WEB_CMD_DEVICE_REMOVE_CANCEL = 7,
    WEB_CMD_PERMIT_JOIN = 8,
    /* Экраны Display (docs/clients/DISPLAY.md): CRUD group / group_item. */
    WEB_CMD_GROUP_PUT = 9,
    WEB_CMD_GROUP_REMOVE = 10,
    WEB_CMD_GROUP_ITEM_PUT = 11,
    WEB_CMD_GROUP_ITEM_REMOVE = 12,
    WEB_CMD_LOCATION_PUT = 13,
    /*
     * Семантическая команда (аддитивно, без WS v3): браузер шлёт property+action+value,
     * ZCL-кодировку делает Web через semantics. Стабильный LE DTO (без C enum/union):
     *   @0  u64 device_uid
     *   @8  u8  endpoint
     *   @9  u16 property_id  (ha_property_id_t)
     *   @11 u8  action_id    (ha_action_id_t)
     *   @12 u8  value_kind    (HA_COMMAND_VALUE_*)
     *   value: NONE — нет; SCALAR @13 u8 kind + @14 u32 bits; XY @13 f32 x + @17 f32 y
     */
    WEB_CMD_SEMANTIC_COMMAND = 14,
    /*
     * Semantic-правило (аддитивно): браузер шлёт логическую модель, Web компилирует её в
     * physical 144-байтную запись. LE, переменной длины:
     *   u64 id, u8 enabled, u8 trigger_kind
     *   trigger: EVENT{u64 device_uid,u8 event_id} | TIME{u16 minutes,u8 mask} |
     *            STATE{u64 device_uid,u8 ep,u16 property,u8 op,u8 edge,f32 value,f32 value2}
     *   u8 conditions_count; conditions[]{u64 uid,u8 ep,u16 property,u8 op,f32 value,f32 value2}
     *   action{u64 uid,u8 ep,u16 property,u8 action,u8 value_kind[,value]}
     */
    WEB_CMD_SEMANTIC_AUTOMATION_PUT = 15,
} web_cmd_t;

typedef struct __attribute__((packed)) {
    uint8_t ver;
    uint8_t type;
    uint16_t len;
    uint16_t seq;
    uint16_t flags;
} web_hdr_t;

/*
 * Собирает кадр (заголовок + payload) в out. Возвращает полную длину или 0, если
 * не влез. payload может быть NULL при len == 0.
 */
size_t web_frame_encode(uint8_t *out, size_t out_size, uint8_t type, uint16_t seq,
                        const void *payload, uint16_t len);

/*
 * Разбирает принятый буфер на заголовок и payload-view. false — битый/обрезанный
 * или чужой версии. payload указывает внутрь buf.
 */
bool web_frame_decode(const uint8_t *buf, size_t len, web_hdr_t *out_hdr, const uint8_t **out_payload,
                      size_t *out_payload_len);
