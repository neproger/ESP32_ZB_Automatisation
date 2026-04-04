#pragma once

#include <stdint.h>

#include "gw_proto/gw_proto_types.h"

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
 */

#define GW_PROTO_VERSION_V1 1u

#if defined(__GNUC__)
#define GW_PROTO_PACKED __attribute__((packed))
#else
#define GW_PROTO_PACKED
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
    GW_PROTO_MSG_CMD_STATE_SYNC = 0x6E,
    GW_PROTO_MSG_EVENT_ZB = 0x6F,
    GW_PROTO_MSG_EVENT_TRACE = 0x70,
    GW_PROTO_MSG_LINK_ACK = 0x71,
    GW_PROTO_MSG_CMD_FACTORY_RESET = 0x72,
} gw_proto_msg_type_t;

typedef enum {
    GW_PROTO_SYNC_SCOPE_FULL = 1,
    GW_PROTO_SYNC_SCOPE_DEVICES = 2,
    GW_PROTO_SYNC_SCOPE_GROUPS = 3,
    GW_PROTO_SYNC_SCOPE_SETTINGS = 4,
    GW_PROTO_SYNC_SCOPE_AUTOMATIONS = 5,
} gw_proto_sync_scope_t;

#define GW_AUTOMATION_ID_MAX   32
#define GW_AUTOMATION_NAME_MAX 48

#define GW_AUTO_MAX_TRIGGERS           8
#define GW_AUTO_MAX_CONDITIONS         16
#define GW_AUTO_MAX_ACTIONS            16
#define GW_AUTO_MAX_STRING_TABLE_BYTES 512

typedef enum {
    GW_AUTO_EVT_ZIGBEE_COMMAND = 1,
    GW_AUTO_EVT_ZIGBEE_ATTR_REPORT = 2,
    GW_AUTO_EVT_DEVICE_JOIN = 3,
    GW_AUTO_EVT_DEVICE_LEAVE = 4,
} gw_auto_evt_type_t;

typedef enum {
    GW_PROTO_EVENT_ATTR_REPORT = 1,
    GW_PROTO_EVENT_COMMAND = 2,
    GW_PROTO_EVENT_DEVICE_JOIN = 3,
    GW_PROTO_EVENT_DEVICE_LEAVE = 4,
    GW_PROTO_EVENT_NET_STATE = 5,
    GW_PROTO_EVENT_DEVICE_ANNCE = 6,
    GW_PROTO_EVENT_LEAVE_INDICATION = 7,
    GW_PROTO_EVENT_DEVICE_UPDATE = 8,
    GW_PROTO_EVENT_DEVICE_AUTHORIZED = 9,
} gw_proto_event_id_t;

typedef enum {
    GW_PROTO_EVENT_VALUE_NONE = 0,
    GW_PROTO_EVENT_VALUE_BOOL = 1,
    GW_PROTO_EVENT_VALUE_I64 = 2,
    GW_PROTO_EVENT_VALUE_F32 = 3,
    GW_PROTO_EVENT_VALUE_TEXT = 4,
} gw_proto_event_value_type_t;

typedef enum {
    GW_PROTO_EVENT_FLAG_REJOIN = 1 << 0,
} gw_proto_event_flag_t;

typedef enum {
    GW_PROTO_TRACE_RULES_FIRED = 1,
    GW_PROTO_TRACE_RULES_ACTION = 2,
} gw_proto_trace_kind_t;

typedef struct GW_PROTO_PACKED {
    uint8_t v;
    uint8_t kind;
    uint8_t ok;
    uint8_t reserved0;
    uint32_t id;
    uint64_t ts_ms;
    char device_uid[GW_DEVICE_UID_STRLEN];
    uint16_t short_addr;
    uint16_t action_index;
    char automation_id[GW_AUTOMATION_ID_MAX];
    char error_text[96];
} gw_proto_trace_v1_t;

typedef enum {
    GW_AUTO_OP_EQ = 1,
    GW_AUTO_OP_NE = 2,
    GW_AUTO_OP_GT = 3,
    GW_AUTO_OP_LT = 4,
    GW_AUTO_OP_GE = 5,
    GW_AUTO_OP_LE = 6,
} gw_auto_op_t;

typedef enum {
    GW_AUTO_VAL_F64 = 1,
    GW_AUTO_VAL_BOOL = 2,
} gw_auto_val_type_t;

typedef enum {
    GW_AUTO_ACT_DEVICE = 1,
    GW_AUTO_ACT_GROUP = 2,
    GW_AUTO_ACT_SCENE = 3,
    GW_AUTO_ACT_BIND = 4,
    GW_AUTO_ACT_MGMT = 5,
} gw_auto_act_kind_t;

typedef enum {
    GW_AUTO_ACT_FLAG_UNBIND = 1 << 0,
    GW_AUTO_ACT_FLAG_REJOIN = 1 << 1,
} gw_auto_act_flag_t;

typedef struct GW_PROTO_PACKED {
    uint8_t event_type;
    uint8_t endpoint;
    uint16_t reserved;
    uint32_t device_uid_off;
    uint32_t cmd_off;
    uint16_t cluster_id;
    uint16_t attr_id;
} gw_auto_bin_trigger_v2_t;

typedef struct GW_PROTO_PACKED {
    uint8_t op;
    uint8_t val_type;
    uint8_t endpoint;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t device_uid_off;
    uint32_t key_off;
    union {
        double f64;
        uint8_t b;
    } v;
} gw_auto_bin_condition_v2_t;

typedef struct GW_PROTO_PACKED {
    uint8_t kind;
    uint8_t endpoint;
    uint8_t aux_ep;
    uint8_t flags;
    uint16_t u16_0;
    uint16_t u16_1;
    uint32_t cmd_off;
    uint32_t uid_off;
    uint32_t uid2_off;
    uint32_t arg0_u32;
    uint32_t arg1_u32;
    uint32_t arg2_u32;
} gw_auto_bin_action_v2_t;

typedef struct GW_PROTO_PACKED {
    char id[GW_AUTOMATION_ID_MAX];
    char name[GW_AUTOMATION_NAME_MAX];
    uint8_t enabled;
    uint8_t reserved;
    uint8_t triggers_count;
    uint8_t conditions_count;
    uint8_t actions_count;
    uint8_t reserved2;
    gw_auto_bin_trigger_v2_t triggers[GW_AUTO_MAX_TRIGGERS];
    gw_auto_bin_condition_v2_t conditions[GW_AUTO_MAX_CONDITIONS];
    gw_auto_bin_action_v2_t actions[GW_AUTO_MAX_ACTIONS];
    uint16_t string_table_size;
    char string_table[GW_AUTO_MAX_STRING_TABLE_BYTES];
} gw_automation_entry_t;

#ifdef __cplusplus
static_assert(sizeof(gw_automation_entry_t) <= 4096,
              "gw_automation_entry_t too large; review stack usage and storage limits");
#else
_Static_assert(sizeof(gw_automation_entry_t) <= 4096,
               "gw_automation_entry_t too large; review stack usage and storage limits");
#endif

typedef struct GW_PROTO_PACKED {
    char id[GW_AUTOMATION_ID_MAX];
    char name[GW_AUTOMATION_NAME_MAX];
    uint8_t enabled;
} gw_automation_meta_t;

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
    char id[GW_AUTOMATION_ID_MAX];
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
    char id[GW_AUTOMATION_ID_MAX];
    uint8_t enabled;
    uint8_t reserved[3];
} gw_proto_cmd_automation_set_enabled_v1_t;

typedef struct GW_PROTO_PACKED {
    char id[GW_AUTOMATION_ID_MAX];
} gw_proto_cmd_automation_remove_v1_t;

typedef struct GW_PROTO_PACKED {
    uint8_t reserved[4];
} gw_proto_cmd_automation_reset_all_v1_t;

typedef struct GW_PROTO_PACKED {
    uint8_t reserved[4];
} gw_proto_cmd_factory_reset_v1_t;

typedef struct GW_PROTO_PACKED {
    uint16_t request_seq;
    int32_t status;
} gw_proto_cmd_result_v1_t;

typedef struct GW_PROTO_PACKED {
    uint32_t event_id;
    uint64_t ts_ms;
    uint8_t event_id_kind;
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
    /*
     * Extra typed fields for Zigbee runtime/system events.
     *
     * Semantics depend on event_id_kind:
     * - DEVICE_UPDATE:
     *     status_code = update status
     *     aux_u16 = tc_action
     *     parent_short_addr = parent short address
     * - DEVICE_AUTHORIZED:
     *     status_code = authorization_status
     *     aux_u16 = authorization_type
     * - LEAVE_INDICATION:
     *     flags bit0 = rejoin
     */
    uint16_t status_code;
    uint16_t aux_u16;
    uint16_t parent_short_addr;
    uint8_t flags;
    uint8_t reserved0;
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
