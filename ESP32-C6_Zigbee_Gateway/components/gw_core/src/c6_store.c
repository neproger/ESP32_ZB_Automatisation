#include "gw_core/c6_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "micro_db/micro_db_core.h"
#include "micro_db/micro_db_flash.h"

static const char *TAG = "gw_c6_store";

typedef struct {
    gw_device_uid_t device_uid;
    uint16_t short_addr;
    char name[32];
    uint64_t last_seen_ms;
    uint8_t has_onoff;
    uint8_t has_button;
    uint8_t reserved[6];
} gw_c6_device_record_t;

typedef struct {
    uint64_t uid_num;
} gw_c6_uid_key_t;

static micro_db_table_t s_device_table;
static micro_db_table_t s_endpoint_table;
static micro_db_table_t s_deleted_table;
static bool s_initialized;

static bool uid_to_u64(const char *uid, uint64_t *out)
{
    if (!uid || !out) {
        return false;
    }
    const char *p = uid;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (*p == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long long value = strtoull(p, &end, 16);
    if (!end || *end != '\0') {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static uint64_t uid_fallback_hash(const char *uid)
{
    const uint8_t *p = (const uint8_t *)uid;
    uint64_t h = 1469598103934665603ull;
    while (p && *p) {
        h ^= (uint64_t)(*p++);
        h *= 1099511628211ull;
    }
    return h ? h : 1ull;
}

static uint64_t uid_to_key_num(const gw_device_uid_t *uid)
{
    uint64_t num = 0;
    if (!uid || uid->uid[0] == '\0') {
        return 0;
    }
    if (uid_to_u64(uid->uid, &num)) {
        return num;
    }
    return uid_fallback_hash(uid->uid);
}

static bool uid_key_equals(const void *lhs_key, const void *rhs_key)
{
    return ((const gw_c6_uid_key_t *)lhs_key)->uid_num == ((const gw_c6_uid_key_t *)rhs_key)->uid_num;
}

static bool endpoint_key_equals(const void *lhs_key, const void *rhs_key)
{
    const gw_c6_endpoint_key_t *lhs = (const gw_c6_endpoint_key_t *)lhs_key;
    const gw_c6_endpoint_key_t *rhs = (const gw_c6_endpoint_key_t *)rhs_key;
    return lhs->uid_num == rhs->uid_num && lhs->endpoint == rhs->endpoint;
}

static bool device_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_c6_device_record_t)) == 0;
}

static bool endpoint_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_proto_endpoint_v1_t)) == 0;
}

static bool deleted_record_equals(const void *lhs_record, const void *rhs_record)
{
    return memcmp(lhs_record, rhs_record, sizeof(gw_deleted_device_t)) == 0;
}

static void device_key_of(const void *record, void *out_key)
{
    const gw_c6_device_record_t *r = (const gw_c6_device_record_t *)record;
    gw_c6_uid_key_t *key = (gw_c6_uid_key_t *)out_key;
    key->uid_num = uid_to_key_num(&r->device_uid);
}

static void endpoint_key_of(const void *record, void *out_key)
{
    const gw_proto_endpoint_v1_t *r = (const gw_proto_endpoint_v1_t *)record;
    gw_c6_endpoint_key_t *key = (gw_c6_endpoint_key_t *)out_key;
    key->uid_num = uid_to_key_num(&r->uid);
    key->endpoint = r->endpoint;
}

static void deleted_key_of(const void *record, void *out_key)
{
    const gw_deleted_device_t *r = (const gw_deleted_device_t *)record;
    gw_c6_uid_key_t *key = (gw_c6_uid_key_t *)out_key;
    key->uid_num = uid_to_key_num(&r->device_uid);
}

static const micro_db_table_schema_t s_device_schema = {
    .name = "c6_device",
    .record_size = sizeof(gw_c6_device_record_t),
    .key_size = sizeof(gw_c6_uid_key_t),
    .max_records = GW_DEVICE_MAX_DEVICES,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_FLASH,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "devices",
    .key_of = device_key_of,
    .key_equals = uid_key_equals,
    .record_equals = device_record_equals,
};

static const micro_db_table_schema_t s_endpoint_schema = {
    .name = "c6_endpoint",
    .record_size = sizeof(gw_proto_endpoint_v1_t),
    .key_size = sizeof(gw_c6_endpoint_key_t),
    .max_records = GW_DEVICE_MAX_DEVICES * GW_DEVICE_MAX_ENDPOINTS,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_FLASH,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "endpoints",
    .key_of = endpoint_key_of,
    .key_equals = endpoint_key_equals,
    .record_equals = endpoint_record_equals,
};

static const micro_db_table_schema_t s_deleted_schema = {
    .name = "c6_deleted",
    .record_size = sizeof(gw_deleted_device_t),
    .key_size = sizeof(gw_c6_uid_key_t),
    .max_records = GW_DEVICE_MAX_DEVICES,
    .backing = MICRO_DB_BACKING_RAM | MICRO_DB_BACKING_FLASH,
    .flags = MICRO_DB_TABLE_F_VERSIONED,
    .persist_key = "deleted_devices",
    .key_of = deleted_key_of,
    .key_equals = uid_key_equals,
    .record_equals = deleted_record_equals,
};

static void device_record_to_public(const gw_c6_device_record_t *src, gw_device_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->device_uid = src->device_uid;
    dst->short_addr = src->short_addr;
    strlcpy(dst->name, src->name, sizeof(dst->name));
    dst->last_seen_ms = src->last_seen_ms;
    dst->has_onoff = (src->has_onoff != 0);
    dst->has_button = (src->has_button != 0);
}

static void endpoint_record_to_public(const gw_proto_endpoint_v1_t *src, gw_zb_endpoint_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->uid = src->uid;
    dst->short_addr = src->short_addr;
    dst->endpoint = src->endpoint;
    dst->profile_id = src->profile_id;
    dst->device_id = src->device_id;
    dst->in_cluster_count = src->in_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : src->in_cluster_count;
    dst->out_cluster_count = src->out_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : src->out_cluster_count;
    if (dst->in_cluster_count > 0) {
        memcpy(dst->in_clusters, src->in_clusters, dst->in_cluster_count * sizeof(uint16_t));
    }
    if (dst->out_cluster_count > 0) {
        memcpy(dst->out_clusters, src->out_clusters, dst->out_cluster_count * sizeof(uint16_t));
    }
}

static void assign_default_name_if_needed(gw_c6_device_record_t *record)
{
    if (!record || record->name[0] != '\0') {
        return;
    }

    const char *prefix = "device";
    if (record->has_button) {
        prefix = "switch";
    } else if (record->has_onoff) {
        prefix = "relay";
    }

    uint32_t max_num = 0;
    const size_t count = micro_db_table_count(&s_device_table);
    for (size_t i = 0; i < count; ++i) {
        gw_c6_device_record_t item = {0};
        if (micro_db_table_get_by_index(&s_device_table, i, &item) != ESP_OK) {
            continue;
        }
        if (strncmp(item.name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        const char *num_str = item.name + strlen(prefix);
        char *end = NULL;
        long num = strtol(num_str, &end, 10);
        if (end > num_str && num > 0 && num <= 999 && (uint32_t)num > max_num) {
            max_num = (uint32_t)num;
        }
    }

    snprintf(record->name, sizeof(record->name), "%s%u", prefix, (unsigned)(max_num + 1));
}

static size_t collect_endpoints_for_uid(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    if (!uid || !out_eps || max_eps == 0) {
        return 0;
    }

    size_t written = 0;
    const size_t count = micro_db_table_count(&s_endpoint_table);
    const uint64_t uid_num = uid_to_key_num(uid);
    for (size_t i = 0; i < count && written < max_eps; ++i) {
        gw_proto_endpoint_v1_t record = {0};
        if (micro_db_table_get_by_index(&s_endpoint_table, i, &record) != ESP_OK) {
            continue;
        }
        if (uid_to_key_num(&record.uid) != uid_num) {
            continue;
        }
        endpoint_record_to_public(&record, &out_eps[written++]);
    }
    return written;
}

esp_err_t gw_c6_store_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(micro_db_flash_init(), TAG, "micro_db_flash_init failed");
    ESP_RETURN_ON_ERROR(micro_db_table_init(&s_device_table, &s_device_schema), TAG, "device table init failed");
    ESP_RETURN_ON_ERROR(micro_db_table_init(&s_endpoint_table, &s_endpoint_schema), TAG, "endpoint table init failed");
    ESP_RETURN_ON_ERROR(micro_db_table_init(&s_deleted_table, &s_deleted_schema), TAG, "deleted table init failed");
    s_initialized = true;
    ESP_LOGI(TAG,
             "store ready: devices=%u endpoints=%u deleted=%u",
             (unsigned)micro_db_table_count(&s_device_table),
             (unsigned)micro_db_table_count(&s_endpoint_table),
             (unsigned)micro_db_table_count(&s_deleted_table));
    return ESP_OK;
}

esp_err_t gw_c6_store_device_upsert(const gw_device_t *device)
{
    if (!s_initialized || !device || device->device_uid.uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    gw_c6_uid_key_t key = {.uid_num = uid_to_key_num(&device->device_uid)};
    gw_c6_device_record_t record = {0};
    if (micro_db_table_get(&s_device_table, &key, &record) != ESP_OK) {
        memset(&record, 0, sizeof(record));
        record.device_uid = device->device_uid;
    }

    record.short_addr = device->short_addr;
    if (device->name[0] != '\0') {
        strlcpy(record.name, device->name, sizeof(record.name));
    }
    if (device->last_seen_ms != 0) {
        record.last_seen_ms = device->last_seen_ms;
    }
    if (device->has_onoff) {
        record.has_onoff = 1u;
    }
    if (device->has_button) {
        record.has_button = 1u;
    }
    assign_default_name_if_needed(&record);
    return micro_db_table_upsert(&s_device_table, &record, NULL, NULL);
}

esp_err_t gw_c6_store_device_get(const gw_device_uid_t *uid, gw_device_t *out_device)
{
    if (!s_initialized || !uid || !out_device || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_c6_device_record_t record = {0};
    ESP_RETURN_ON_ERROR(micro_db_table_get(&s_device_table, &(gw_c6_uid_key_t){.uid_num = uid_to_key_num(uid)}, &record), TAG, "device not found");
    device_record_to_public(&record, out_device);
    return ESP_OK;
}

esp_err_t gw_c6_store_device_get_full(const gw_device_uid_t *uid, gw_device_full_t *out_device)
{
    if (!s_initialized || !uid || !out_device || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    gw_c6_device_record_t record = {0};
    esp_err_t err = micro_db_table_get(&s_device_table, &(gw_c6_uid_key_t){.uid_num = uid_to_key_num(uid)}, &record);
    if (err != ESP_OK) {
        return err;
    }

    memset(out_device, 0, sizeof(*out_device));
    out_device->device_uid = record.device_uid;
    out_device->short_addr = record.short_addr;
    strlcpy(out_device->name, record.name, sizeof(out_device->name));
    out_device->last_seen_ms = record.last_seen_ms;
    out_device->has_onoff = (record.has_onoff != 0);
    out_device->has_button = (record.has_button != 0);

    gw_zb_endpoint_t live_eps[GW_DEVICE_MAX_ENDPOINTS] = {0};
    size_t count = collect_endpoints_for_uid(uid, live_eps, GW_DEVICE_MAX_ENDPOINTS);
    for (size_t i = 0; i < count; ++i) {
        if (live_eps[i].endpoint == 0 || live_eps[i].endpoint > GW_DEVICE_MAX_ENDPOINTS) {
            continue;
        }
        size_t slot = (size_t)(live_eps[i].endpoint - 1u);
        gw_device_endpoint_t *dst = &out_device->endpoints[slot];
        dst->profile_id = live_eps[i].profile_id;
        dst->device_id = live_eps[i].device_id;
        dst->in_cluster_count = live_eps[i].in_cluster_count;
        dst->out_cluster_count = live_eps[i].out_cluster_count;
        if (dst->in_cluster_count > 0) {
            memcpy(dst->in_clusters, live_eps[i].in_clusters, dst->in_cluster_count * sizeof(uint16_t));
        }
        if (dst->out_cluster_count > 0) {
            memcpy(dst->out_clusters, live_eps[i].out_clusters, dst->out_cluster_count * sizeof(uint16_t));
        }
        if (live_eps[i].endpoint > out_device->endpoint_count) {
            out_device->endpoint_count = live_eps[i].endpoint;
        }
    }

    return ESP_OK;
}

esp_err_t gw_c6_store_device_get_full_by_short(uint16_t short_addr, gw_device_full_t *out_device)
{
    if (!s_initialized || !out_device || short_addr == 0 || short_addr == 0xFFFF) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t count = micro_db_table_count(&s_device_table);
    for (size_t i = 0; i < count; ++i) {
        gw_c6_device_record_t record = {0};
        if (micro_db_table_get_by_index(&s_device_table, i, &record) != ESP_OK) {
            continue;
        }
        if (record.short_addr == short_addr) {
            return gw_c6_store_device_get_full(&record.device_uid, out_device);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t gw_c6_store_device_get_full_by_index(size_t index, gw_device_full_t *out_device)
{
    if (!s_initialized || !out_device) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_c6_device_record_t record = {0};
    ESP_RETURN_ON_ERROR(micro_db_table_get_by_index(&s_device_table, index, &record), TAG, "device index not found");
    return gw_c6_store_device_get_full(&record.device_uid, out_device);
}

esp_err_t gw_c6_store_device_set_name(const gw_device_uid_t *uid, const char *name)
{
    if (!s_initialized || !uid || !name || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_c6_device_record_t record = {0};
    gw_c6_uid_key_t key = {.uid_num = uid_to_key_num(uid)};
    ESP_RETURN_ON_ERROR(micro_db_table_get(&s_device_table, &key, &record), TAG, "device not found");
    strlcpy(record.name, name, sizeof(record.name));
    return micro_db_table_upsert(&s_device_table, &record, NULL, NULL);
}

esp_err_t gw_c6_store_endpoint_remove_device(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    bool removed_any = false;
    while (true) {
        bool removed_one = false;
        const size_t count = micro_db_table_count(&s_endpoint_table);
        for (size_t i = 0; i < count; ++i) {
            gw_proto_endpoint_v1_t record = {0};
            if (micro_db_table_get_by_index(&s_endpoint_table, i, &record) != ESP_OK) {
                continue;
            }
            if (uid_to_key_num(&record.uid) != uid_to_key_num(uid)) {
                continue;
            }
            bool removed = false;
            (void)micro_db_table_remove(&s_endpoint_table,
                                        &(gw_c6_endpoint_key_t){.uid_num = uid_to_key_num(uid), .endpoint = record.endpoint},
                                        &removed);
            removed_any = removed_any || removed;
            removed_one = removed;
            break;
        }
        if (!removed_one) {
            break;
        }
    }
    return removed_any ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t gw_c6_store_device_remove(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    (void)gw_c6_store_endpoint_remove_device(uid);
    return micro_db_table_remove(&s_device_table, &(gw_c6_uid_key_t){.uid_num = uid_to_key_num(uid)}, NULL);
}

size_t gw_c6_store_device_count(void)
{
    return s_initialized ? micro_db_table_count(&s_device_table) : 0;
}

size_t gw_c6_store_device_list(gw_device_t *out_devices, size_t max_devices)
{
    if (!s_initialized || !out_devices || max_devices == 0) {
        return 0;
    }
    size_t written = 0;
    const size_t count = micro_db_table_count(&s_device_table);
    for (size_t i = 0; i < count && written < max_devices; ++i) {
        gw_c6_device_record_t record = {0};
        if (micro_db_table_get_by_index(&s_device_table, i, &record) != ESP_OK) {
            continue;
        }
        device_record_to_public(&record, &out_devices[written++]);
    }
    return written;
}

size_t gw_c6_store_device_list_full(gw_device_full_t *out_devices, size_t max_devices)
{
    if (!s_initialized || !out_devices || max_devices == 0) {
        return 0;
    }
    size_t written = 0;
    const size_t count = micro_db_table_count(&s_device_table);
    for (size_t i = 0; i < count && written < max_devices; ++i) {
        gw_c6_device_record_t record = {0};
        if (micro_db_table_get_by_index(&s_device_table, i, &record) != ESP_OK) {
            continue;
        }
        if (gw_c6_store_device_get_full(&record.device_uid, &out_devices[written]) == ESP_OK) {
            written++;
        }
    }
    return written;
}

esp_err_t gw_c6_store_device_sync_endpoints(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_c6_device_record_t record = {0};
    ESP_RETURN_ON_ERROR(micro_db_table_get(&s_device_table, &(gw_c6_uid_key_t){.uid_num = uid_to_key_num(uid)}, &record), TAG, "device not found");

    gw_zb_endpoint_t eps[GW_DEVICE_MAX_ENDPOINTS] = {0};
    size_t count = collect_endpoints_for_uid(uid, eps, GW_DEVICE_MAX_ENDPOINTS);
    record.has_onoff = 0u;
    for (size_t i = 0; i < count; ++i) {
        for (size_t ci = 0; ci < eps[i].in_cluster_count; ++ci) {
            if (eps[i].in_clusters[ci] == 0x0006) {
                record.has_onoff = 1u;
                break;
            }
        }
        if (record.has_onoff) {
            break;
        }
    }
    assign_default_name_if_needed(&record);
    return micro_db_table_upsert(&s_device_table, &record, NULL, NULL);
}

esp_err_t gw_c6_store_endpoint_upsert(const gw_zb_endpoint_t *endpoint)
{
    if (!s_initialized || !endpoint || endpoint->uid.uid[0] == '\0' || endpoint->endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_endpoint_v1_t record = {0};
    record.uid = endpoint->uid;
    record.short_addr = endpoint->short_addr;
    record.endpoint = endpoint->endpoint;
    record.version = 0;
    record.profile_id = endpoint->profile_id;
    record.device_id = endpoint->device_id;
    record.in_cluster_count = endpoint->in_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : endpoint->in_cluster_count;
    record.out_cluster_count = endpoint->out_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : endpoint->out_cluster_count;
    if (record.in_cluster_count > 0) {
        memcpy(record.in_clusters, endpoint->in_clusters, record.in_cluster_count * sizeof(uint16_t));
    }
    if (record.out_cluster_count > 0) {
        memcpy(record.out_clusters, endpoint->out_clusters, record.out_cluster_count * sizeof(uint16_t));
    }
    return micro_db_table_upsert(&s_endpoint_table, &record, NULL, NULL);
}

size_t gw_c6_store_endpoint_list(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return collect_endpoints_for_uid(uid, out_eps, max_eps);
}

size_t gw_c6_store_endpoint_list_all(gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    if (!s_initialized || !out_eps || max_eps == 0) {
        return 0;
    }
    size_t written = 0;
    const size_t count = micro_db_table_count(&s_endpoint_table);
    for (size_t i = 0; i < count && written < max_eps; ++i) {
        gw_proto_endpoint_v1_t record = {0};
        if (micro_db_table_get_by_index(&s_endpoint_table, i, &record) != ESP_OK) {
            continue;
        }
        endpoint_record_to_public(&record, &out_eps[written++]);
    }
    return written;
}

bool gw_c6_store_find_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid)
{
    if (!s_initialized || !out_uid || short_addr == 0 || short_addr == 0xFFFF) {
        return false;
    }
    const size_t count = micro_db_table_count(&s_endpoint_table);
    for (size_t i = 0; i < count; ++i) {
        gw_proto_endpoint_v1_t record = {0};
        if (micro_db_table_get_by_index(&s_endpoint_table, i, &record) != ESP_OK) {
            continue;
        }
        if (record.short_addr == short_addr && record.uid.uid[0] != '\0') {
            *out_uid = record.uid;
            return true;
        }
    }
    return false;
}

esp_err_t gw_c6_store_deleted_add(const gw_device_uid_t *uid, uint64_t removed_at_ms)
{
    if (!s_initialized || !uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_deleted_device_t item = {.device_uid = *uid, .removed_at_ms = removed_at_ms};
    return micro_db_table_upsert(&s_deleted_table, &item, NULL, NULL);
}

esp_err_t gw_c6_store_deleted_remove(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return micro_db_table_remove(&s_deleted_table, &(gw_c6_uid_key_t){.uid_num = uid_to_key_num(uid)}, NULL);
}

bool gw_c6_store_deleted_contains(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid || uid->uid[0] == '\0') {
        return false;
    }
    gw_deleted_device_t item = {0};
    return micro_db_table_get(&s_deleted_table, &(gw_c6_uid_key_t){.uid_num = uid_to_key_num(uid)}, &item) == ESP_OK;
}

size_t gw_c6_store_deleted_count(void)
{
    return s_initialized ? micro_db_table_count(&s_deleted_table) : 0;
}
