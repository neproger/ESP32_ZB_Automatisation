#include "gw_store/gw_store_schema.h"

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
    gw_store_endpoint_key_t *key = (gw_store_endpoint_key_t *)out_key;
    memset(key, 0, sizeof(*key));
    key->uid = r->uid;
    key->endpoint = r->endpoint;
}

static bool topology_endpoint_key_equals(const void *lhs_key, const void *rhs_key)
{
    return memcmp(lhs_key, rhs_key, sizeof(gw_store_endpoint_key_t)) == 0;
}

static bool topology_endpoint_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_proto_endpoint_v1_t)) == 0;
}

const micro_db_table_schema_t GW_STORE_SCHEMA_TOPOLOGY_DEVICE = {
    .name = "topology_device",
    .record_size = sizeof(gw_proto_device_v1_t),
    .key_size = sizeof(gw_device_uid_t),
    .max_records = GW_DEVICE_MAX_DEVICES,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_FLASH,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "devices_v2",
    .key_of = topology_device_key_of,
    .key_equals = topology_device_key_equals,
    .record_equals = topology_device_record_equals,
};

const micro_db_table_schema_t GW_STORE_SCHEMA_TOPOLOGY_ENDPOINT = {
    .name = "topology_endpoint",
    .record_size = sizeof(gw_proto_endpoint_v1_t),
    .key_size = sizeof(gw_store_endpoint_key_t),
    .max_records = GW_DEVICE_MAX_DEVICES * GW_DEVICE_MAX_ENDPOINTS,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_FLASH,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "endpoints_v2",
    .key_of = topology_endpoint_key_of,
    .key_equals = topology_endpoint_key_equals,
    .record_equals = topology_endpoint_record_equals,
};
