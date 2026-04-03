#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_DEVICE_UID_STRLEN 19

typedef struct {
    char uid[GW_DEVICE_UID_STRLEN];
} gw_device_uid_t;

typedef struct {
    gw_device_uid_t device_uid;
    uint16_t short_addr;
    uint8_t endpoint;
} gw_device_ref_t;

#define GW_GROUP_ID_MAX   32
#define GW_GROUP_NAME_MAX 48

typedef struct {
    char id[GW_GROUP_ID_MAX];
    char name[GW_GROUP_NAME_MAX];
    uint32_t version;
    uint32_t created_at_ms;
    uint32_t updated_at_ms;
} gw_group_entry_t;

typedef struct {
    char group_id[GW_GROUP_ID_MAX];
    gw_device_uid_t device_uid;
    uint8_t endpoint;
    uint8_t reserved[3];
    uint32_t version;
    uint32_t order;
    char label[32];
} gw_group_item_t;

#define GW_ZB_MAX_CLUSTERS 16
#define GW_STATE_KEY_MAX   24
#define GW_STATE_TEXT_MAX  64

typedef enum {
    GW_STATE_VALUE_BOOL = 1,
    GW_STATE_VALUE_F32 = 2,
    GW_STATE_VALUE_U32 = 3,
    GW_STATE_VALUE_U64 = 4,
    GW_STATE_VALUE_TEXT = 5,
} gw_state_value_type_t;

#ifdef __cplusplus
}
#endif
