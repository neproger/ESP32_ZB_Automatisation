#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Web transport protocol: binary, little-endian. Transport тупой — он несёт байты
 * canonical-модели и не интерпретирует их.
 *
 * Envelope (payload кадра):
 *   UPSERT: [type][key][record]     — type = ha_entity_t (стабильный ABI), key/record —
 *                                     canonical struct verbatim (см. ha_model)
 *   REMOVE: [type][key]
 *   EVENT:  [WEB_MSG_EVENT][ha_event_t]
 *
 * `type` = числовое значение `ha_entity_t` (canonical entity type ABI). Значения
 * передаются и персистятся, поэтому НИКОГДА не переиспользуются/не переставляются.
 * Управляющие кадры (SYNC_..., ACK, EVENT, COMMAND) выбраны вне диапазона ha_entity_t.
 *
 * C-struct передаётся как wire ABI: fixed-width поля, явный reserved, reserved = 0,
 * little-endian (contract ESP32/browser), layout защищён static_assert в ha_model.
 */

#define WEB_PROTO_VER 3u
#define WEB_PROTO_HDR_SIZE 8u

/* Максимальный payload: type + самый большой key+record (CAPABILITIES 8+216). */
#define WEB_PROTO_MAX_PAYLOAD 256u
#define WEB_PROTO_MAX_FRAME (WEB_PROTO_HDR_SIZE + WEB_PROTO_MAX_PAYLOAD)

typedef enum {
    WEB_MSG_SYNC_BEGIN = 0x01,
    WEB_MSG_SYNC_END = 0x02,
    /* browser → device: подтверждение кадра с данным header.seq (payload пуст). */
    WEB_MSG_ACK = 0x03,
    /* device → browser: payload = ha_event_t verbatim. */
    WEB_MSG_EVENT = 0x10,
    /* browser → device: payload = [cmd][args] (args — canonical struct verbatim). */
    WEB_MSG_COMMAND = 0x20,
} web_msg_t;

/*
 * Команды browser → device. Args — canonical struct verbatim (без перекодирования):
 *   PROPERTY_COMMAND       = ha_property_command_t
 *   ENTITY_RENAME          = ha_entity_rename_t
 *   AUTOMATION_PUT         = ha_automation_key_t + ha_rule_t
 *   AUTOMATION_REMOVE      = ha_automation_key_t
 *   GROUP_PUT              = ha_group_key_t + ha_group_record_t
 *   GROUP_REMOVE           = ha_group_key_t
 *   GROUP_ITEM_PUT         = ha_group_item_key_t + ha_group_item_record_t
 *   GROUP_ITEM_REMOVE      = ha_group_item_key_t
 *   LOCATION_PUT           = ha_device_id_t + ha_location_record_t
 *   DEVICE_RENAME          = ha_device_id_t + char name[32]
 *   LOGICAL_DEVICE_REMOVE  = ha_device_id_t
 *   DISCOVERY              = u8 seconds (0 — stop)
 */
typedef enum {
    WEB_CMD_SNAPSHOT = 1,
    WEB_CMD_AUTOMATION_PUT = 4,
    WEB_CMD_AUTOMATION_REMOVE = 5,
    WEB_CMD_DISCOVERY = 8,
    WEB_CMD_GROUP_PUT = 9,
    WEB_CMD_GROUP_REMOVE = 10,
    WEB_CMD_LOCATION_PUT = 13,
    WEB_CMD_GROUP_ITEM_PUT = 16,
    WEB_CMD_GROUP_ITEM_REMOVE = 17,
    WEB_CMD_ENTITY_RENAME = 18,
    WEB_CMD_PROPERTY_COMMAND = 19,
    WEB_CMD_DEVICE_RENAME = 20,
    WEB_CMD_LOGICAL_DEVICE_REMOVE = 21,
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
