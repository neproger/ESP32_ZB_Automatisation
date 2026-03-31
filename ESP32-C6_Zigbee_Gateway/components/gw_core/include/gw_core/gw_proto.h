#pragma once

#include <stdint.h>

#include "gw_core/state_store.h"
#include "gw_core/types.h"
#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_PROTO_VERSION_V1 1u

#if defined(__GNUC__)
#define GW_PROTO_PACKED __attribute__((packed))
#else
#define GW_PROTO_PACKED
#endif

#ifndef GW_STATE_TEXT_MAX
#define GW_STATE_TEXT_MAX 64
#endif

#ifndef GW_GROUP_ID_MAX
#define GW_GROUP_ID_MAX 32
#endif

#ifndef GW_GROUP_NAME_MAX
#define GW_GROUP_NAME_MAX 48
#endif

typedef struct GW_PROTO_PACKED {
    uint8_t version;
    uint8_t type;
    uint16_t len;
    uint16_t seq;
    uint16_t reserved;
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
    GW_PROTO_MSG_AUTOMATION_UPSERT = 0x4E,
    GW_PROTO_MSG_AUTOMATION_REMOVE = 0x4F,
    GW_PROTO_MSG_CMD_PERMIT_JOIN = 0x50,
    GW_PROTO_MSG_CMD_DEVICE_RENAME = 0x51,
    GW_PROTO_MSG_CMD_DEVICE_REMOVE = 0x52,
    GW_PROTO_MSG_CMD_DEVICE_REMOVE_ALL = 0x53,
    GW_PROTO_MSG_CMD_GROUP_CREATE = 0x54,
    GW_PROTO_MSG_CMD_GROUP_RENAME = 0x55,
    GW_PROTO_MSG_CMD_GROUP_DELETE = 0x56,
    GW_PROTO_MSG_CMD_GROUP_ITEM_SET = 0x57,
    GW_PROTO_MSG_CMD_GROUP_ITEM_REMOVE = 0x58,
    GW_PROTO_MSG_CMD_GROUP_ITEM_REORDER = 0x59,
    GW_PROTO_MSG_CMD_GROUP_ITEM_LABEL = 0x5A,
    GW_PROTO_MSG_CMD_SETTINGS_SET = 0x5B,
    GW_PROTO_MSG_CMD_AUTOMATION_SET_ENABLED = 0x5C,
    GW_PROTO_MSG_CMD_AUTOMATION_REMOVE = 0x5D,
    GW_PROTO_MSG_CMD_AUTOMATION_RESET_ALL = 0x5E,
    GW_PROTO_MSG_CMD_AUTOMATION_SAVE = 0x5F,
    GW_PROTO_MSG_CMD_ACTION_EXEC = 0x60,
    GW_PROTO_MSG_CMD_RESULT = 0x61,
    GW_PROTO_MSG_CMD_WIFI_CONFIG_SET = 0x62,
    GW_PROTO_MSG_CMD_NET_SERVICES_START = 0x63,
    GW_PROTO_MSG_CMD_READ_ATTR = 0x64,
    GW_PROTO_MSG_CMD_ONOFF = 0x65,
    GW_PROTO_MSG_CMD_LEVEL = 0x66,
    GW_PROTO_MSG_CMD_COLOR_XY = 0x67,
    GW_PROTO_MSG_CMD_COLOR_TEMP = 0x68,
    GW_PROTO_MSG_CMD_IDENTIFY = 0x69,
    GW_PROTO_MSG_CMD_BIND = 0x6A,
    GW_PROTO_MSG_CMD_UNBIND = 0x6B,
    GW_PROTO_MSG_CMD_SCENE_STORE = 0x6C,
    GW_PROTO_MSG_CMD_SCENE_RECALL = 0x6D,
    GW_PROTO_MSG_EVENT_ZB = 0x6E,
} gw_proto_msg_type_t;

typedef enum {
    GW_PROTO_EVENT_ATTR_REPORT = 1,
    GW_PROTO_EVENT_COMMAND = 2,
    GW_PROTO_EVENT_DEVICE_JOIN = 3,
    GW_PROTO_EVENT_DEVICE_LEAVE = 4,
    GW_PROTO_EVENT_NET_STATE = 5,
} gw_proto_event_id_t;

typedef enum {
    GW_PROTO_EVENT_VALUE_NONE = 0,
    GW_PROTO_EVENT_VALUE_BOOL = 1,
    GW_PROTO_EVENT_VALUE_I64 = 2,
    GW_PROTO_EVENT_VALUE_F32 = 3,
    GW_PROTO_EVENT_VALUE_TEXT = 4,
} gw_proto_event_value_type_t;

typedef enum {
    GW_PROTO_SYNC_SCOPE_FULL = 1,
    GW_PROTO_SYNC_SCOPE_DEVICES = 2,
    GW_PROTO_SYNC_SCOPE_GROUPS = 3,
    GW_PROTO_SYNC_SCOPE_SETTINGS = 4,
} gw_proto_sync_scope_t;

typedef struct GW_PROTO_PACKED {
    uint8_t scope;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t total_records;
} gw_proto_sync_begin_v1_t;

typedef struct GW_PROTO_PACKED {
    uint8_t scope;
    uint8_t status;
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
    uint8_t value_type;
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

typedef struct GW_PROTO_PACKED {
    char id[32];
} gw_proto_automation_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    uint8_t seconds;
    uint8_t reserved[3];
} gw_proto_cmd_permit_join_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    char name[32];
} gw_proto_cmd_device_rename_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
} gw_proto_cmd_device_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    uint8_t reserved[4];
} gw_proto_cmd_device_remove_all_v1_t;

typedef struct GW_PROTO_PACKED {
    char id[GW_GROUP_ID_MAX];
    char name[GW_GROUP_NAME_MAX];
} gw_proto_cmd_group_create_v1_t;

typedef struct GW_PROTO_PACKED {
    char id[GW_GROUP_ID_MAX];
    char name[GW_GROUP_NAME_MAX];
} gw_proto_cmd_group_rename_v1_t;

typedef struct GW_PROTO_PACKED {
    char id[GW_GROUP_ID_MAX];
} gw_proto_cmd_group_delete_v1_t;

typedef struct GW_PROTO_PACKED {
    char group_id[GW_GROUP_ID_MAX];
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved[3];
} gw_proto_cmd_group_item_set_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved[3];
} gw_proto_cmd_group_item_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    char group_id[GW_GROUP_ID_MAX];
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved0[3];
    uint32_t order;
} gw_proto_cmd_group_item_reorder_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved[3];
    char label[32];
} gw_proto_cmd_group_item_label_v1_t;

typedef struct GW_PROTO_PACKED {
    char id[32];
    uint8_t enabled;
    uint8_t reserved[3];
} gw_proto_cmd_automation_set_enabled_v1_t;

typedef struct GW_PROTO_PACKED {
    char id[32];
} gw_proto_cmd_automation_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    uint8_t reserved[4];
} gw_proto_cmd_automation_reset_all_v1_t;

typedef struct GW_PROTO_PACKED {
    uint16_t request_seq;
    int32_t status;
} gw_proto_cmd_result_v1_t;

typedef struct GW_PROTO_PACKED {
    uint32_t event_id;
    uint64_t ts_ms;
    uint8_t event_id_kind;
    char event_type[32];
    char cmd[16];
    gw_device_uid_t device_uid;
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t value_type;
    uint8_t value_bool;
    int64_t value_i64;
    float value_f32;
    char value_text[24];
} gw_proto_event_v1_t;

typedef struct GW_PROTO_PACKED {
    char ssid[33];
    char password[65];
} gw_proto_cmd_wifi_config_set_v1_t;

typedef struct GW_PROTO_PACKED {
    uint8_t reserved[4];
} gw_proto_cmd_net_services_start_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved0;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint16_t reserved1;
} gw_proto_cmd_read_attr_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t cmd;
    uint16_t reserved0;
} gw_proto_cmd_onoff_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t level;
    uint16_t transition_ds;
    uint16_t reserved0;
} gw_proto_cmd_level_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved0;
    uint16_t x;
    uint16_t y;
    uint16_t transition_ds;
} gw_proto_cmd_color_xy_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved0;
    uint16_t mireds;
    uint16_t transition_ds;
    uint16_t reserved1;
} gw_proto_cmd_color_temp_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t identify_seconds;
    uint16_t reserved0;
} gw_proto_cmd_identify_v1_t;

typedef struct GW_PROTO_PACKED {
    gw_device_uid_t src_uid;
    uint8_t src_endpoint;
    uint8_t dst_endpoint;
    uint16_t cluster_id;
    gw_device_uid_t dst_uid;
} gw_proto_cmd_bind_v1_t;

typedef gw_proto_cmd_bind_v1_t gw_proto_cmd_unbind_v1_t;

typedef struct GW_PROTO_PACKED {
    uint16_t group_id;
    uint8_t scene_id;
    uint8_t reserved0;
} gw_proto_cmd_scene_store_v1_t;

typedef struct GW_PROTO_PACKED {
    uint16_t group_id;
    uint8_t scene_id;
    uint8_t reserved0;
} gw_proto_cmd_scene_recall_v1_t;

#ifdef __cplusplus
}
#endif
