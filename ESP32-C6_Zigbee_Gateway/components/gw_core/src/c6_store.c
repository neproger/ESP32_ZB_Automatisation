#include "gw_core/c6_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "gw_store/gw_store_topology.h"
#include "micro_db/micro_db_core.h"
#include "micro_db/micro_db_flash.h"

static const char *TAG = "gw_c6_store";
static bool s_initialized;

static size_t collect_endpoints_for_uid(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps);

static const char *status_name(gw_device_status_t status)
{
    switch (status) {
        case GW_DEVICE_STATUS_NONE:
            return "none";
        case GW_DEVICE_STATUS_NEW:
            return "new";
        case GW_DEVICE_STATUS_DISCOVERING:
            return "discovering";
        case GW_DEVICE_STATUS_READY:
            return "ready";
        case GW_DEVICE_STATUS_REMOVING:
            return "removing";
        case GW_DEVICE_STATUS_REMOVED:
            return "removed";
        default:
            return "unknown";
    }
}

static bool uid_is_valid_topology_uid(const gw_device_uid_t *uid)
{
    if (!uid || uid->uid[0] == '\0') {
        return false;
    }

    const char *s = uid->uid;
    if (!(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
        return false;
    }
    s += 2;
    if (*s == '\0') {
        return false;
    }

    for (; *s != '\0'; ++s) {
        const char c = *s;
        const bool is_hex =
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F');
        if (!is_hex) {
            return false;
        }
    }
    return true;
}

static void log_store_dump(const char *phase)
{
    const size_t count = gw_store_count_devices();
    ESP_LOGW(TAG, "store dump phase=%s devices=%u endpoints=%u", phase ? phase : "?", (unsigned)count, (unsigned)gw_store_count_endpoints());
    for (size_t i = 0; i < count; ++i) {
        gw_proto_device_v1_t record = {0};
        if (gw_store_get_device_by_index(i, &record) != ESP_OK) {
            continue;
        }

        gw_zb_endpoint_t eps[GW_DEVICE_MAX_ENDPOINTS] = {0};
        const size_t ep_count = collect_endpoints_for_uid(&record.device_uid, eps, GW_DEVICE_MAX_ENDPOINTS);
        ESP_LOGW(TAG,
                 "store[%u] uid=%s short=0x%04x status=%s live_eps=%u name=%s",
                 (unsigned)i,
                 record.device_uid.uid,
                 (unsigned)record.short_addr,
                 status_name((gw_device_status_t)record.status),
                 (unsigned)ep_count,
                 record.name);
    }
}

static void normalize_persisted_device_statuses(void)
{
    gw_device_uid_t remove_list[GW_DEVICE_MAX_DEVICES] = {0};
    size_t remove_count = 0;

    const size_t count = gw_store_count_devices();
    for (size_t i = 0; i < count; ++i) {
        gw_proto_device_v1_t record = {0};
        if (gw_store_get_device_by_index(i, &record) != ESP_OK) {
            continue;
        }

        gw_zb_endpoint_t eps[GW_DEVICE_MAX_ENDPOINTS] = {0};
        const size_t ep_count = collect_endpoints_for_uid(&record.device_uid, eps, GW_DEVICE_MAX_ENDPOINTS);
        const bool has_real_endpoints = (ep_count > 0);
        const gw_device_status_t status = (gw_device_status_t)record.status;

        const bool invalid_uid = !uid_is_valid_topology_uid(&record.device_uid);
        const bool invalid_status = (status < GW_DEVICE_STATUS_NONE || status > GW_DEVICE_STATUS_REMOVED);
        const bool invalid_short = (record.short_addr == 0xFFFFu);

        if (invalid_uid || invalid_status || (invalid_short && !has_real_endpoints)) {
            if (remove_count < GW_DEVICE_MAX_DEVICES) {
                remove_list[remove_count++] = record.device_uid;
            }
            ESP_LOGW(TAG,
                     "scrub persisted corrupt device uid=%s short=0x%04x status=%u endpoints=%u",
                     record.device_uid.uid,
                     (unsigned)record.short_addr,
                     (unsigned)record.status,
                     (unsigned)ep_count);
            continue;
        }

        if (!has_real_endpoints) {
            continue;
        }

        if (status == GW_DEVICE_STATUS_DISCOVERING || status == GW_DEVICE_STATUS_NEW) {
            record.status = (uint8_t)GW_DEVICE_STATUS_READY;
            (void)gw_store_upsert_device(&record, NULL, NULL);
            ESP_LOGW(TAG,
                     "normalized persisted status uid=%s short=0x%04x %s->ready endpoints=%u",
                     record.device_uid.uid,
                     (unsigned)record.short_addr,
                     status_name(status),
                     (unsigned)ep_count);
        }
    }

    for (size_t i = 0; i < remove_count; ++i) {
        bool removed = false;
        (void)gw_store_remove_full_device(&remove_list[i], &removed);
    }
}

static void proto_device_to_public(const gw_proto_device_v1_t *src, gw_device_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->device_uid = src->device_uid;
    dst->short_addr = src->short_addr;
    strlcpy(dst->name, src->name, sizeof(dst->name));
    dst->last_seen_ms = src->last_seen_ms;
    dst->has_onoff = (src->has_onoff != 0);
    dst->has_button = (src->has_button != 0);
    dst->status = (gw_device_status_t)src->status;
}

static void public_device_to_proto(const gw_device_t *src, gw_proto_device_v1_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->device_uid = src->device_uid;
    dst->short_addr = src->short_addr;
    strlcpy(dst->name, src->name, sizeof(dst->name));
    dst->last_seen_ms = src->last_seen_ms;
    dst->has_onoff = src->has_onoff ? 1u : 0u;
    dst->has_button = src->has_button ? 1u : 0u;
    dst->status = (uint8_t)(src->status != GW_DEVICE_STATUS_NONE ? src->status : GW_DEVICE_STATUS_NEW);
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

static void public_endpoint_to_proto(const gw_zb_endpoint_t *src, gw_proto_endpoint_v1_t *dst)
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

static void assign_default_name_if_needed(gw_proto_device_v1_t *record)
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
    const size_t count = gw_store_count_devices();
    for (size_t i = 0; i < count; ++i) {
        gw_proto_device_v1_t item = {0};
        if (gw_store_get_device_by_index(i, &item) != ESP_OK) {
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

typedef struct {
    gw_zb_endpoint_t *out_eps;
    size_t max_eps;
    size_t written;
} endpoint_collect_ctx_t;

static bool collect_endpoint_cb(const void *record, void *user_ctx)
{
    const gw_proto_endpoint_v1_t *endpoint = (const gw_proto_endpoint_v1_t *)record;
    endpoint_collect_ctx_t *ctx = (endpoint_collect_ctx_t *)user_ctx;
    if (!ctx || ctx->written >= ctx->max_eps) {
        return false;
    }
    endpoint_record_to_public(endpoint, &ctx->out_eps[ctx->written++]);
    return true;
}

static size_t collect_endpoints_for_uid(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    if (!uid || !out_eps || max_eps == 0) {
        return 0;
    }

    endpoint_collect_ctx_t ctx = {
        .out_eps = out_eps,
        .max_eps = max_eps,
        .written = 0,
    };
    (void)gw_store_iter_endpoints_for_device(uid, collect_endpoint_cb, &ctx);
    return ctx.written;
}

esp_err_t gw_c6_store_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(micro_db_flash_init(), TAG, "micro_db_flash_init failed");
    ESP_RETURN_ON_ERROR(gw_store_init_topology(), TAG, "topology init failed");
    normalize_persisted_device_statuses();
    s_initialized = true;
    ESP_LOGI(TAG,
             "store ready: devices=%u endpoints=%u",
             (unsigned)gw_store_count_devices(),
             (unsigned)gw_store_count_endpoints());
    log_store_dump("post_init");
    return ESP_OK;
}

esp_err_t gw_c6_store_device_upsert(const gw_device_t *device)
{
    if (!s_initialized || !device || device->device_uid.uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_device_v1_t record = {0};
    if (gw_store_get_device(&device->device_uid, &record) != ESP_OK) {
        public_device_to_proto(device, &record);
    } else {
        if (device->short_addr != 0) {
            record.short_addr = device->short_addr;
        }
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
        if (device->status != GW_DEVICE_STATUS_NONE) {
            record.status = (uint8_t)device->status;
        }
    }

    assign_default_name_if_needed(&record);
    return gw_store_upsert_device(&record, NULL, NULL);
}

esp_err_t gw_c6_store_device_get(const gw_device_uid_t *uid, gw_device_t *out_device)
{
    if (!s_initialized || !uid || !out_device || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_device_v1_t record = {0};
    ESP_RETURN_ON_ERROR(gw_store_get_device(uid, &record), TAG, "device not found");
    proto_device_to_public(&record, out_device);
    return ESP_OK;
}

esp_err_t gw_c6_store_device_get_full(const gw_device_uid_t *uid, gw_device_full_t *out_device)
{
    if (!s_initialized || !uid || !out_device || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_device_v1_t record = {0};
    ESP_RETURN_ON_ERROR(gw_store_get_device(uid, &record), TAG, "device not found");

    memset(out_device, 0, sizeof(*out_device));
    out_device->device_uid = record.device_uid;
    out_device->short_addr = record.short_addr;
    strlcpy(out_device->name, record.name, sizeof(out_device->name));
    out_device->last_seen_ms = record.last_seen_ms;
    out_device->has_onoff = (record.has_onoff != 0);
    out_device->has_button = (record.has_button != 0);
    out_device->status = (gw_device_status_t)record.status;

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

esp_err_t gw_c6_store_device_set_status(const gw_device_uid_t *uid, gw_device_status_t status)
{
    if (!s_initialized || !uid || !uid->uid[0] || status == GW_DEVICE_STATUS_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_device_v1_t record = {0};
    ESP_RETURN_ON_ERROR(gw_store_get_device(uid, &record), TAG, "device not found");
    record.status = (uint8_t)status;
    return gw_store_upsert_device(&record, NULL, NULL);
}

esp_err_t gw_c6_store_device_get_full_by_short(uint16_t short_addr, gw_device_full_t *out_device)
{
    if (!s_initialized || !out_device || short_addr == 0 || short_addr == 0xFFFF) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_uid_t uid = {0};
    if (gw_store_find_device_uid_by_short(short_addr, &uid)) {
        return gw_c6_store_device_get_full(&uid, out_device);
    }

    const size_t count = gw_store_count_devices();
    for (size_t i = 0; i < count; ++i) {
        gw_proto_device_v1_t record = {0};
        if (gw_store_get_device_by_index(i, &record) != ESP_OK) {
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
    gw_proto_device_v1_t record = {0};
    ESP_RETURN_ON_ERROR(gw_store_get_device_by_index(index, &record), TAG, "device index not found");
    return gw_c6_store_device_get_full(&record.device_uid, out_device);
}

esp_err_t gw_c6_store_device_set_name(const gw_device_uid_t *uid, const char *name)
{
    if (!s_initialized || !uid || !name || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_device_v1_t record = {0};
    ESP_RETURN_ON_ERROR(gw_store_get_device(uid, &record), TAG, "device not found");
    strlcpy(record.name, name, sizeof(record.name));
    return gw_store_upsert_device(&record, NULL, NULL);
}

typedef struct {
    gw_store_endpoint_key_t *keys;
    size_t max_keys;
    size_t count;
} endpoint_key_collect_ctx_t;

static bool collect_endpoint_key_cb(const void *record, void *user_ctx)
{
    const gw_proto_endpoint_v1_t *endpoint = (const gw_proto_endpoint_v1_t *)record;
    endpoint_key_collect_ctx_t *ctx = (endpoint_key_collect_ctx_t *)user_ctx;
    if (!ctx || ctx->count >= ctx->max_keys) {
        return false;
    }
    ctx->keys[ctx->count].uid = endpoint->uid;
    ctx->keys[ctx->count].endpoint = endpoint->endpoint;
    ctx->count++;
    return true;
}

esp_err_t gw_c6_store_endpoint_remove_device(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t cap = gw_store_count_endpoints();
    if (cap == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    gw_store_endpoint_key_t *keys = (gw_store_endpoint_key_t *)calloc(cap, sizeof(gw_store_endpoint_key_t));
    if (!keys) {
        return ESP_ERR_NO_MEM;
    }

    endpoint_key_collect_ctx_t ctx = {
        .keys = keys,
        .max_keys = cap,
        .count = 0,
    };
    (void)gw_store_iter_endpoints_for_device(uid, collect_endpoint_key_cb, &ctx);

    bool removed_any = false;
    for (size_t i = 0; i < ctx.count; ++i) {
        bool removed = false;
        (void)gw_store_remove_endpoint(&keys[i], &removed);
        removed_any = removed_any || removed;
    }

    free(keys);
    return removed_any ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t gw_c6_store_device_remove(const gw_device_uid_t *uid)
{
    if (!s_initialized || !uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return gw_store_remove_full_device(uid, NULL);
}

size_t gw_c6_store_device_count(void)
{
    return s_initialized ? gw_store_count_devices() : 0;
}

size_t gw_c6_store_device_list(gw_device_t *out_devices, size_t max_devices)
{
    if (!s_initialized || !out_devices || max_devices == 0) {
        return 0;
    }
    size_t written = 0;
    const size_t count = gw_store_count_devices();
    for (size_t i = 0; i < count && written < max_devices; ++i) {
        gw_proto_device_v1_t record = {0};
        if (gw_store_get_device_by_index(i, &record) != ESP_OK) {
            continue;
        }
        proto_device_to_public(&record, &out_devices[written++]);
    }
    return written;
}

size_t gw_c6_store_device_list_full(gw_device_full_t *out_devices, size_t max_devices)
{
    if (!s_initialized || !out_devices || max_devices == 0) {
        return 0;
    }
    size_t written = 0;
    const size_t count = gw_store_count_devices();
    for (size_t i = 0; i < count && written < max_devices; ++i) {
        gw_proto_device_v1_t record = {0};
        if (gw_store_get_device_by_index(i, &record) != ESP_OK) {
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

    gw_proto_device_v1_t record = {0};
    ESP_RETURN_ON_ERROR(gw_store_get_device(uid, &record), TAG, "device not found");

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
    return gw_store_upsert_device(&record, NULL, NULL);
}

esp_err_t gw_c6_store_endpoint_upsert(const gw_zb_endpoint_t *endpoint)
{
    if (!s_initialized || !endpoint || endpoint->uid.uid[0] == '\0' || endpoint->endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_endpoint_v1_t record = {0};
    public_endpoint_to_proto(endpoint, &record);
    return gw_store_upsert_endpoint(&record, NULL, NULL);
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
    const size_t count = gw_store_count_endpoints();
    for (size_t i = 0; i < count && written < max_eps; ++i) {
        gw_proto_endpoint_v1_t record = {0};
        if (gw_store_get_endpoint_by_index(i, &record) != ESP_OK) {
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
    return gw_store_find_device_uid_by_short(short_addr, out_uid);
}
