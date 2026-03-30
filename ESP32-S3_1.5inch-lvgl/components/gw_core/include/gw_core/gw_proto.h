#pragma once

#include <stdint.h>

#include "gw_core/state_store.h"
#include "gw_core/types.h"
#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Canonical binary protocol for embedded transports.
 *
 * Little-endian on wire.
 * Transport framing is separate:
 * - UART: SOF + hdr + payload + CRC
 * - WebSocket binary: hdr + payload
 *
 * JS/browser is expected to parse payloads from ArrayBuffer/DataView.
 */

#define GW_PROTO_VERSION_V1 1u

#if defined(__GNUC__)
#define GW_PROTO_PACKED __attribute__((packed))
#else
#define GW_PROTO_PACKED
#endif

typedef struct GW_PROTO_PACKED {
    uint8_t version;   /* GW_PROTO_VERSION_V1 */
    uint8_t type;      /* gw_proto_msg_type_t */
    uint16_t len;      /* payload size in bytes */
    uint16_t seq;      /* request/response or stream sequence */
    uint16_t reserved; /* keep header aligned and extensible */
} gw_proto_hdr_t;

typedef enum {
    GW_PROTO_MSG_NONE = 0,

    GW_PROTO_MSG_SYNC_BEGIN = 0x40,
    GW_PROTO_MSG_SYNC_END = 0x41,

    GW_PROTO_MSG_DEVICE_UPSERT = 0x42,
    GW_PROTO_MSG_DEVICE_REMOVE = 0x43,

    GW_PROTO_MSG_ENDPOINT_UPSERT = 0x44,
    GW_PROTO_MSG_ENDPOINT_REMOVE = 0x45,

    GW_PROTO_MSG_STATE_ITEM = 0x46,
    GW_PROTO_MSG_STATE_REMOVE = 0x47,

    GW_PROTO_MSG_GROUP_UPSERT = 0x48,
    GW_PROTO_MSG_GROUP_REMOVE = 0x49,

    GW_PROTO_MSG_GROUP_ITEM_UPSERT = 0x4A,
    GW_PROTO_MSG_GROUP_ITEM_REMOVE = 0x4B,

    GW_PROTO_MSG_SETTINGS = 0x4C,
    GW_PROTO_MSG_SNAPSHOT_REQUEST = 0x4D,
} gw_proto_msg_type_t;

typedef enum {
    GW_PROTO_SYNC_SCOPE_FULL = 1,
    GW_PROTO_SYNC_SCOPE_DEVICES = 2,
    GW_PROTO_SYNC_SCOPE_GROUPS = 3,
    GW_PROTO_SYNC_SCOPE_SETTINGS = 4,
} gw_proto_sync_scope_t;

typedef struct GW_PROTO_PACKED {
    uint8_t scope;      /* gw_proto_sync_scope_t */
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t total_records;
} gw_proto_sync_begin_v1_t;

typedef struct GW_PROTO_PACKED {
    uint8_t scope;      /* gw_proto_sync_scope_t */
    uint8_t status;     /* 0=ok */
    uint16_t reserved0;
    uint32_t total_records;
} gw_proto_sync_end_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
} gw_proto_device_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint16_t short_addr;
    char name[32];
    uint32_t version;
    uint64_t last_seen_ms;
    uint8_t has_onoff;
    uint8_t has_button;
    uint8_t reserved[6];
} gw_proto_device_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t uid;
    uint8_t endpoint;
    uint8_t reserved0;
    uint16_t short_addr;
} gw_proto_endpoint_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t uid;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t reserved0;
    uint32_t version;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t in_cluster_count;
    uint8_t out_cluster_count;
    uint16_t in_clusters[GW_ZB_MAX_CLUSTERS];
    uint16_t out_clusters[GW_ZB_MAX_CLUSTERS];
} gw_proto_endpoint_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t uid;
    uint8_t endpoint;
    char key[GW_STATE_KEY_MAX];
} gw_proto_state_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t uid;
    uint8_t endpoint;
    uint8_t value_type;  /* gw_state_value_type_t */
    uint16_t reserved0;
    char key[GW_STATE_KEY_MAX];
    uint32_t version;
    uint8_t value_bool;
    uint8_t reserved1[3];
    float value_f32;
    uint32_t value_u32;
    uint64_t value_u64;
    char value_text[GW_STATE_TEXT_MAX];
    uint64_t ts_ms;
} gw_proto_state_item_v1_t;

typedef struct GW_PROTO_PACKED {
    char id[GW_GROUP_ID_MAX];
} gw_proto_group_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    char id[GW_GROUP_ID_MAX];
    char name[GW_GROUP_NAME_MAX];
    uint32_t version;
    uint32_t created_at_ms;
    uint32_t updated_at_ms;
} gw_proto_group_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved[3];
} gw_proto_group_item_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    char group_id[GW_GROUP_ID_MAX];
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved0[3];
    uint32_t version;
    uint32_t order;
    char label[32];
} gw_proto_group_item_v1_t;

typedef struct GW_PROTO_PACKED {
    uint32_t screensaver_timeout_ms;
    uint32_t weather_success_interval_ms;
    uint32_t weather_retry_interval_ms;
    uint8_t timezone_auto;
    uint8_t reserved0;
    int16_t timezone_offset_min;
} gw_proto_settings_v1_t;

#ifdef __cplusplus
}
#endif
