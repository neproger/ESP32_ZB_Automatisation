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
     * Canonical group item (A4): identity = (group_id, entity_id, property_id). Хранимая
     * запись уже entity-native, мост не нужен.
     * ITEM (52): u64 group_id @0, u64 entity @8, u16 property @16, u16 order @18, title[32] @20.
     * REMOVE (18): u64 group_id @0, u64 entity @8, u16 property @16.
     */
    WEB_MSG_SEMANTIC_GROUP_ITEM = 0x19,
    WEB_MSG_SEMANTIC_GROUP_ITEM_REMOVE = 0x1A,
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
    /* Экраны Display (docs/clients/DISPLAY.md): CRUD group. Виджеты — entity-native
     * (WEB_CMD_SEMANTIC_GROUP_ITEM_PUT/REMOVE ниже). */
    WEB_CMD_GROUP_PUT = 9,
    WEB_CMD_GROUP_REMOVE = 10,
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
    /* Canonical group item: PUT (52): group_id,entity,property,order,title[32];
     * REMOVE (18): group_id,entity,property. Ключ entity-native (A4). */
    WEB_CMD_SEMANTIC_GROUP_ITEM_PUT = 16,
    WEB_CMD_SEMANTIC_GROUP_ITEM_REMOVE = 17,
    /* Переименование Entity (Шаг A, A6.1): u64 entity_id @0, char name[32] @8. Adapter не
     * участвует — имя принадлежит Entity. */
    WEB_CMD_ENTITY_RENAME = 18,
    /*
     * Canonical команда по entity (Шаг A, A6.1c): u64 entity_id @0, u16 property @8, u8 action
     * @10, u8 value_kind @11; SCALAR @12 u8 kind + u32 bits; XY @12 f32 x + f32 y. Backend →
     * HA_CMD_PROPERTY (adapter резолвит entity→addr, мост кодирует). Без uid/ep/cluster/attr.
     */
    WEB_CMD_PROPERTY_COMMAND = 19,
} web_cmd_t;

/*
 * Web transport v3 (Шаг A, A6.0): canonical-контракт для клиента. Обычный v3 НЕ несёт
 * uid/endpoint/cluster/attr/zcl_type — identity это entity_id. Живёт рядом с v2
 * (transitional) до A6.x: кадр тот же, v3 отличается набором типов. Здесь только
 * read-сущности идентичности; команды/остальное — на A6.2+.
 *
 * Раскладки (LE, без C enum/union на проводе):
 *   ENTITY (40):                u64 entity_id @0, char name[32] @8
 *   ENTITY_REMOVE (8):          u64 entity_id @0
 *   PROPERTY_STATE (15):        u64 entity_id @0, u16 property @8, u8 value_kind @10, u32 bits @11
 *   PROPERTY_STATE_REMOVE (10): u64 entity_id @0, u16 property @8
 *   CAPABILITIES (перем.):      u64 entity_id @0, u8 count @8,
 *                               props[]{ u16 property, u8 action_count, u8 actions[] }
 *   CAPABILITIES_REMOVE (8):    u64 entity_id @0
 */
typedef enum {
    WEB_MSG3_ENTITY = 0x30,
    WEB_MSG3_ENTITY_REMOVE = 0x31,
    WEB_MSG3_PROPERTY_STATE = 0x32,
    WEB_MSG3_PROPERTY_STATE_REMOVE = 0x33,
    WEB_MSG3_CAPABILITIES = 0x34,
    WEB_MSG3_CAPABILITIES_REMOVE = 0x35,
    /*
     * Canonical Automation (A6.2): entity-native правило (ha_rule_t). Переменная длина:
     *   u64 id, u8 enabled, u8 kind; trigger{EVENT u64 entity+u8 event_id | TIME u16 min+u8
     *   mask | STATE u64 entity+u16 property+u8 op+u8 edge+f32+f32}; u8 cond_count;
     *   conditions[]{u64 entity,u16 property,u8 op,f32,f32}; action{u64 entity,u16 property,
     *   u8 action,u8 value_kind[,scalar u8 kind+u32 | xy f32+f32]}. REMOVE: u64 id.
     * Совпадает с WEB_CMD_AUTOMATION_PUT.
     */
    WEB_MSG3_AUTOMATION = 0x36,
    WEB_MSG3_AUTOMATION_REMOVE = 0x37,
    /*
     * Canonical event (A6.2): источник — Entity, без uid/endpoint/raw payload.
     * DTO (14): u64 source_entity @0, u8 event_id @8, u8 value_kind @9, u32 value_bits @10.
     */
    WEB_MSG3_EVENT = 0x38,
} web_msg3_t;

#define WEB_MSG3_ENTITY_SIZE 40u /* u64 entity_id + char name[HA_ENTITY_NAME_MAX=32] */
#define WEB_MSG3_PROPERTY_STATE_SIZE 15u
#define WEB_MSG3_PROPERTY_STATE_REMOVE_SIZE 10u

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
