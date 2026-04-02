#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// IEEE (EUI‑64) as string: "0x00124B0012345678" + '\0'
#define GW_DEVICE_UID_STRLEN 19

typedef struct {
    char uid[GW_DEVICE_UID_STRLEN];
} gw_device_uid_t;

typedef struct {
    gw_device_uid_t device_uid; // stable (IEEE)
    uint16_t short_addr;        // current network address (may change after rejoin)
    uint8_t endpoint;
} gw_device_ref_t;

#define GW_DEVICE_MAX_DEVICES 64
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


#ifdef __cplusplus
}
#endif

