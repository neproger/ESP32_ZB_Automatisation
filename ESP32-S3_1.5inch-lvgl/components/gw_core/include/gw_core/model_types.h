#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_proto/gw_proto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_ZB_MAX_ENDPOINTS 64

typedef struct {
    gw_device_uid_t uid;
    uint16_t short_addr;
    uint8_t endpoint;
    uint32_t version;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t in_cluster_count;
    uint8_t out_cluster_count;
    uint16_t in_clusters[GW_ZB_MAX_CLUSTERS];
    uint16_t out_clusters[GW_ZB_MAX_CLUSTERS];
} gw_zb_endpoint_t;

typedef struct {
    gw_device_uid_t device_uid;
    uint16_t short_addr;
    char name[32];
    uint32_t version;
    uint64_t last_seen_ms;
    bool has_onoff;
    bool has_button;
} gw_device_t;

#define GW_STATE_MAX_ITEMS 1024

typedef struct {
    gw_device_uid_t uid;
    uint8_t endpoint;
    char key[GW_STATE_KEY_MAX];
    uint32_t version;
    gw_state_value_type_t value_type;
    bool value_bool;
    float value_f32;
    uint32_t value_u32;
    uint64_t value_u64;
    char value_text[GW_STATE_TEXT_MAX];
    uint64_t ts_ms;
} gw_state_item_t;

#ifdef __cplusplus
}
#endif
