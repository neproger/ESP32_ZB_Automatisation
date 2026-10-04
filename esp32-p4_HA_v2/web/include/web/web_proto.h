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

typedef enum {
    WEB_MSG_SYNC_BEGIN = 0x01,
    WEB_MSG_SYNC_END = 0x02,
    WEB_MSG_ENTITY = 0x10,
    WEB_MSG_ENTITY_REMOVE = 0x11,
    WEB_MSG_COMMAND = 0x20,
    WEB_MSG_CMD_RESULT = 0x21,
} web_msg_t;

typedef enum {
    WEB_CMD_SNAPSHOT = 1,
    WEB_CMD_ZB_COMMAND = 2,
    WEB_CMD_DEVICE_RENAME = 3,
    WEB_CMD_AUTOMATION_PUT = 4,
    WEB_CMD_AUTOMATION_REMOVE = 5,
    WEB_CMD_DEVICE_REMOVE = 6,
    WEB_CMD_DEVICE_REMOVE_CANCEL = 7,
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
