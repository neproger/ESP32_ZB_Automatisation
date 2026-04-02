#pragma once

#include "gw_core/gw_proto.h"
#include "micro_db/micro_db_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_MODEL_TABLE_NONE = 0,
    GW_MODEL_TABLE_TOPOLOGY_DEVICE = 1,
    GW_MODEL_TABLE_TOPOLOGY_ENDPOINT = 2,
    GW_MODEL_TABLE_STATE_ITEM = 3,
    GW_MODEL_TABLE_GROUP = 4,
    GW_MODEL_TABLE_GROUP_ITEM = 5,
    GW_MODEL_TABLE_SETTINGS = 6,
    GW_MODEL_TABLE_AUTOMATION = 7,
} gw_model_table_id_t;

typedef struct {
    char id[GW_GROUP_ID_MAX];
} gw_model_group_key_t;

typedef struct {
    gw_device_uid_t uid;
    uint8_t endpoint;
} gw_model_endpoint_key_t;

typedef struct {
    gw_device_uid_t uid;
    uint8_t endpoint;
    char key[GW_STATE_KEY_MAX];
} gw_model_state_key_t;

extern const micro_db_table_schema_t GW_MODEL_SCHEMA_TOPOLOGY_DEVICE;
extern const micro_db_table_schema_t GW_MODEL_SCHEMA_TOPOLOGY_ENDPOINT;
extern const micro_db_table_schema_t GW_MODEL_SCHEMA_STATE_ITEM;
extern const micro_db_table_schema_t GW_MODEL_SCHEMA_GROUP;
extern const micro_db_table_schema_t GW_MODEL_SCHEMA_GROUP_ITEM;
extern const micro_db_table_schema_t GW_MODEL_SCHEMA_SETTINGS;
extern const micro_db_table_schema_t GW_MODEL_SCHEMA_AUTOMATION;

#ifdef __cplusplus
}
#endif
