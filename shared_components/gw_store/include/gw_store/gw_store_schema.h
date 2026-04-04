#pragma once

#include "gw_proto/gw_proto.h"
#include "micro_db/micro_db_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GW_DEVICE_MAX_DEVICES
#define GW_DEVICE_MAX_DEVICES 64
#endif

#ifndef GW_DEVICE_MAX_ENDPOINTS
#define GW_DEVICE_MAX_ENDPOINTS 8
#endif

typedef struct {
    gw_device_uid_t uid;
    uint8_t endpoint;
} gw_store_endpoint_key_t;

extern const micro_db_table_schema_t GW_STORE_SCHEMA_TOPOLOGY_DEVICE;
extern const micro_db_table_schema_t GW_STORE_SCHEMA_TOPOLOGY_ENDPOINT;

#ifdef __cplusplus
}
#endif
