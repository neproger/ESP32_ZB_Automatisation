#include "gw_model/gw_model_schema.h"

#include <string.h>

static void topology_device_key_of(const void *record, void *out_key)
{
    const gw_proto_device_v1_t *r = (const gw_proto_device_v1_t *)record;
    gw_device_uid_t *key = (gw_device_uid_t *)out_key;
    *key = r->device_uid;
}

static bool topology_device_key_equals(const void *lhs_key, const void *rhs_key)
{
    return memcmp(lhs_key, rhs_key, sizeof(gw_device_uid_t)) == 0;
}

static bool topology_device_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_proto_device_v1_t)) == 0;
}

static void topology_endpoint_key_of(const void *record, void *out_key)
{
    const gw_proto_endpoint_v1_t *r = (const gw_proto_endpoint_v1_t *)record;
    gw_model_endpoint_key_t *key = (gw_model_endpoint_key_t *)out_key;
    memset(key, 0, sizeof(*key));
    key->uid = r->uid;
    key->endpoint = r->endpoint;
}

static bool topology_endpoint_key_equals(const void *lhs_key, const void *rhs_key)
{
    return memcmp(lhs_key, rhs_key, sizeof(gw_model_endpoint_key_t)) == 0;
}

static bool topology_endpoint_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_proto_endpoint_v1_t)) == 0;
}

static void state_item_key_of(const void *record, void *out_key)
{
    const gw_proto_state_item_v1_t *r = (const gw_proto_state_item_v1_t *)record;
    gw_model_state_key_t *key = (gw_model_state_key_t *)out_key;
    memset(key, 0, sizeof(*key));
    key->uid = r->uid;
    key->endpoint = r->endpoint;
    memcpy(key->key, r->key, sizeof(key->key));
}

static bool state_item_key_equals(const void *lhs_key, const void *rhs_key)
{
    return memcmp(lhs_key, rhs_key, sizeof(gw_model_state_key_t)) == 0;
}

static bool state_item_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_proto_state_item_v1_t)) == 0;
}

static void group_key_of(const void *record, void *out_key)
{
    const gw_proto_group_v1_t *r = (const gw_proto_group_v1_t *)record;
    gw_model_group_key_t *key = (gw_model_group_key_t *)out_key;
    memcpy(key->id, r->id, sizeof(key->id));
}

static bool group_key_equals(const void *lhs_key, const void *rhs_key)
{
    return memcmp(lhs_key, rhs_key, sizeof(gw_model_group_key_t)) == 0;
}

static bool group_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_proto_group_v1_t)) == 0;
}

static void group_item_key_of(const void *record, void *out_key)
{
    const gw_proto_group_item_v1_t *r = (const gw_proto_group_item_v1_t *)record;
    gw_model_endpoint_key_t *key = (gw_model_endpoint_key_t *)out_key;
    memset(key, 0, sizeof(*key));
    key->uid = r->device_uid;
    key->endpoint = r->endpoint;
}

static bool group_item_key_equals(const void *lhs_key, const void *rhs_key)
{
    return memcmp(lhs_key, rhs_key, sizeof(gw_model_endpoint_key_t)) == 0;
}

static bool group_item_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_proto_group_item_v1_t)) == 0;
}

static void settings_key_of(const void *record, void *out_key)
{
    (void)record;
    uint8_t *key = (uint8_t *)out_key;
    *key = 1u;
}

static bool settings_key_equals(const void *lhs_key, const void *rhs_key)
{
    return (*(const uint8_t *)lhs_key) == (*(const uint8_t *)rhs_key);
}

static bool settings_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_proto_settings_v1_t)) == 0;
}

typedef struct {
    char id[GW_AUTOMATION_ID_MAX];
} gw_model_automation_key_t;

static void automation_key_of(const void *record, void *out_key)
{
    const gw_automation_entry_t *r = (const gw_automation_entry_t *)record;
    gw_model_automation_key_t *key = (gw_model_automation_key_t *)out_key;
    memset(key, 0, sizeof(*key));
    memcpy(key->id, r->id, sizeof(key->id));
}

static bool automation_key_equals(const void *lhs_key, const void *rhs_key)
{
    return memcmp(lhs_key, rhs_key, sizeof(gw_model_automation_key_t)) == 0;
}

static bool automation_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_automation_entry_t)) == 0;
}

const micro_db_table_schema_t GW_MODEL_SCHEMA_TOPOLOGY_DEVICE = {
    .name = "topology_device",
    .record_size = sizeof(gw_proto_device_v1_t),
    .key_size = sizeof(gw_device_uid_t),
    .max_records = GW_DEVICE_MAX_DEVICES,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_NVS,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "devices",
    .key_of = topology_device_key_of,
    .key_equals = topology_device_key_equals,
    .record_equals = topology_device_record_equals,
};

const micro_db_table_schema_t GW_MODEL_SCHEMA_TOPOLOGY_ENDPOINT = {
    .name = "topology_endpoint",
    .record_size = sizeof(gw_proto_endpoint_v1_t),
    .key_size = sizeof(gw_model_endpoint_key_t),
    .max_records = GW_DEVICE_MAX_DEVICES * GW_ZB_MAX_ENDPOINTS,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_NVS,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "endpoints",
    .key_of = topology_endpoint_key_of,
    .key_equals = topology_endpoint_key_equals,
    .record_equals = topology_endpoint_record_equals,
};

const micro_db_table_schema_t GW_MODEL_SCHEMA_STATE_ITEM = {
    .name = "state_item",
    .record_size = sizeof(gw_proto_state_item_v1_t),
    .key_size = sizeof(gw_model_state_key_t),
    .max_records = GW_STATE_MAX_ITEMS,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_NVS,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "state",
    .key_of = state_item_key_of,
    .key_equals = state_item_key_equals,
    .record_equals = state_item_record_equals,
};

const micro_db_table_schema_t GW_MODEL_SCHEMA_GROUP = {
    .name = "group",
    .record_size = sizeof(gw_proto_group_v1_t),
    .key_size = sizeof(gw_model_group_key_t),
    .max_records = 24,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_NVS,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "groups",
    .key_of = group_key_of,
    .key_equals = group_key_equals,
    .record_equals = group_record_equals,
};

const micro_db_table_schema_t GW_MODEL_SCHEMA_GROUP_ITEM = {
    .name = "group_item",
    .record_size = sizeof(gw_proto_group_item_v1_t),
    .key_size = sizeof(gw_model_endpoint_key_t),
    .max_records = 256,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_NVS,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "group_items",
    .key_of = group_item_key_of,
    .key_equals = group_item_key_equals,
    .record_equals = group_item_record_equals,
};

const micro_db_table_schema_t GW_MODEL_SCHEMA_SETTINGS = {
    .name = "settings",
    .record_size = sizeof(gw_proto_settings_v1_t),
    .key_size = sizeof(uint8_t),
    .max_records = 1,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_NVS,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "settings",
    .key_of = settings_key_of,
    .key_equals = settings_key_equals,
    .record_equals = settings_record_equals,
};

const micro_db_table_schema_t GW_MODEL_SCHEMA_AUTOMATION = {
    .name = "automation",
    .record_size = sizeof(gw_automation_entry_t),
    .key_size = sizeof(gw_model_automation_key_t),
    .max_records = 32,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_NVS,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "autos",
    .key_of = automation_key_of,
    .key_equals = automation_key_equals,
    .record_equals = automation_record_equals,
};
