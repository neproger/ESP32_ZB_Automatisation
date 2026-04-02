#include "gw_model/gw_model_topology.h"

#include <stdlib.h>
#include <string.h>

#include "gw_model/gw_model_state.h"
#include "gw_model_notify.h"

static micro_db_table_t s_device_table;
static micro_db_table_t s_endpoint_table;

typedef struct {
    gw_device_uid_t uid;
    uint32_t head_slot;
    uint8_t state;
} endpoint_owner_bucket_t;

static endpoint_owner_bucket_t *s_endpoint_owners;
static uint32_t *s_endpoint_next_slot;
static size_t s_endpoint_owner_cap;

static void endpoint_owner_attach(const gw_device_uid_t *uid, uint32_t slot);

enum {
    OWNER_EMPTY = 0,
    OWNER_USED = 1,
    OWNER_TOMBSTONE = 2,
};

static uint32_t hash_uid(const gw_device_uid_t *uid)
{
    const uint8_t *p = (const uint8_t *)uid;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(*uid); ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static size_t next_owner_capacity(size_t max_records)
{
    size_t cap = 1;
    const size_t doubled = max_records * 2u;
    const size_t target = doubled > 0 ? doubled : 2u;
    while (cap < target) {
        cap <<= 1u;
    }
    return cap;
}

static void rebuild_endpoint_owner_index(void)
{
    if (!s_endpoint_owners || !s_endpoint_next_slot) {
        return;
    }

    memset(s_endpoint_owners, 0, s_endpoint_owner_cap * sizeof(*s_endpoint_owners));
    for (size_t i = 0; i < GW_DEVICE_MAX_DEVICES * GW_ZB_MAX_ENDPOINTS; ++i) {
        s_endpoint_next_slot[i] = UINT32_MAX;
    }

    for (size_t i = 0; i < micro_db_table_count(&s_endpoint_table); ++i) {
        gw_proto_endpoint_v1_t record = {0};
        if (micro_db_table_get_by_index(&s_endpoint_table, i, &record) != ESP_OK) {
            continue;
        }
        gw_model_endpoint_key_t key = {.uid = record.uid, .endpoint = record.endpoint};
        uint32_t slot = 0;
        if (micro_db_table_get_slot(&s_endpoint_table, &key, &slot) == ESP_OK) {
            endpoint_owner_attach(&record.uid, slot);
        }
    }
}

static int endpoint_owner_find(const gw_device_uid_t *uid, bool *out_found)
{
    const size_t mask = s_endpoint_owner_cap - 1u;
    const uint32_t hash = hash_uid(uid);
    int first_tombstone = -1;

    for (size_t probe = 0; probe < s_endpoint_owner_cap; ++probe) {
        const size_t pos = (hash + probe) & mask;
        const uint8_t state = s_endpoint_owners[pos].state;
        if (state == OWNER_EMPTY) {
            *out_found = false;
            return (first_tombstone >= 0) ? first_tombstone : (int)pos;
        }
        if (state == OWNER_TOMBSTONE) {
            if (first_tombstone < 0) {
                first_tombstone = (int)pos;
            }
            continue;
        }
        if (memcmp(&s_endpoint_owners[pos].uid, uid, sizeof(*uid)) == 0) {
            *out_found = true;
            return (int)pos;
        }
    }

    *out_found = false;
    return first_tombstone;
}

static void endpoint_owner_attach(const gw_device_uid_t *uid, uint32_t slot)
{
    bool found = false;
    const int pos = endpoint_owner_find(uid, &found);
    if (pos < 0) {
        return;
    }

    if (!found) {
        s_endpoint_owners[pos].uid = *uid;
        s_endpoint_owners[pos].head_slot = UINT32_MAX;
        s_endpoint_owners[pos].state = OWNER_USED;
    }

    s_endpoint_next_slot[slot] = s_endpoint_owners[pos].head_slot;
    s_endpoint_owners[pos].head_slot = slot;
}

static void endpoint_owner_detach(const gw_device_uid_t *uid, uint32_t slot)
{
    bool found = false;
    const int pos = endpoint_owner_find(uid, &found);
    if (!found || pos < 0) {
        return;
    }

    uint32_t current = s_endpoint_owners[pos].head_slot;
    uint32_t prev = UINT32_MAX;
    while (current != UINT32_MAX) {
        if (current == slot) {
            const uint32_t next = s_endpoint_next_slot[current];
            if (prev == UINT32_MAX) {
                s_endpoint_owners[pos].head_slot = next;
            } else {
                s_endpoint_next_slot[prev] = next;
            }
            s_endpoint_next_slot[current] = UINT32_MAX;
            if (s_endpoint_owners[pos].head_slot == UINT32_MAX) {
                s_endpoint_owners[pos].state = OWNER_TOMBSTONE;
            }
            return;
        }
        prev = current;
        current = s_endpoint_next_slot[current];
    }
}

typedef struct {
    gw_model_state_key_t *keys;
    size_t count;
    size_t cap;
} state_key_list_t;

static bool collect_state_key_cb(const void *record, void *user_ctx)
{
    const gw_proto_state_item_v1_t *state = (const gw_proto_state_item_v1_t *)record;
    state_key_list_t *list = (state_key_list_t *)user_ctx;

    if (!list || list->count >= list->cap) {
        return false;
    }

    gw_model_state_key_t *dst = &list->keys[list->count++];
    memset(dst, 0, sizeof(*dst));
    dst->uid = state->uid;
    dst->endpoint = state->endpoint;
    memcpy(dst->key, state->key, sizeof(dst->key));
    return true;
}

static void remove_state_for_endpoint(const gw_model_endpoint_key_t *endpoint_key)
{
    if (!endpoint_key) {
        return;
    }

    const size_t cap = gw_model_count_state();
    if (cap == 0) {
        return;
    }

    gw_model_state_key_t *keys = (gw_model_state_key_t *)calloc(cap, sizeof(gw_model_state_key_t));
    if (!keys) {
        return;
    }

    state_key_list_t list = {
        .keys = keys,
        .count = 0,
        .cap = cap,
    };
    (void)gw_model_iter_state_for_endpoint(endpoint_key, collect_state_key_cb, &list);
    for (size_t i = 0; i < list.count; ++i) {
        bool removed = false;
        (void)gw_model_remove_state(&list.keys[i], &removed);
    }

    free(keys);
}

typedef struct {
    gw_model_endpoint_key_t *keys;
    size_t count;
    size_t cap;
} endpoint_key_list_t;

static bool collect_endpoint_key_cb(const void *record, void *user_ctx)
{
    const gw_proto_endpoint_v1_t *endpoint = (const gw_proto_endpoint_v1_t *)record;
    endpoint_key_list_t *list = (endpoint_key_list_t *)user_ctx;

    if (!list || list->count >= list->cap) {
        return false;
    }

    gw_model_endpoint_key_t *dst = &list->keys[list->count++];
    dst->uid = endpoint->uid;
    dst->endpoint = endpoint->endpoint;
    return true;
}

esp_err_t gw_model_init_topology(void)
{
    esp_err_t err = micro_db_table_init(&s_device_table, &GW_MODEL_SCHEMA_TOPOLOGY_DEVICE);
    if (err != ESP_OK) {
        return err;
    }

    err = micro_db_table_init(&s_endpoint_table, &GW_MODEL_SCHEMA_TOPOLOGY_ENDPOINT);
    if (err != ESP_OK) {
        (void)micro_db_table_deinit(&s_device_table);
        return err;
    }

    s_endpoint_owner_cap = next_owner_capacity(GW_DEVICE_MAX_DEVICES * GW_ZB_MAX_ENDPOINTS);
    s_endpoint_owners = (endpoint_owner_bucket_t *)calloc(s_endpoint_owner_cap, sizeof(endpoint_owner_bucket_t));
    s_endpoint_next_slot = (uint32_t *)calloc(GW_DEVICE_MAX_DEVICES * GW_ZB_MAX_ENDPOINTS, sizeof(uint32_t));
    if (!s_endpoint_owners || !s_endpoint_next_slot) {
        free(s_endpoint_next_slot);
        free(s_endpoint_owners);
        s_endpoint_next_slot = NULL;
        s_endpoint_owners = NULL;
        (void)micro_db_table_deinit(&s_endpoint_table);
        (void)micro_db_table_deinit(&s_device_table);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < GW_DEVICE_MAX_DEVICES * GW_ZB_MAX_ENDPOINTS; ++i) {
        s_endpoint_next_slot[i] = UINT32_MAX;
    }

    rebuild_endpoint_owner_index();

    return ESP_OK;
}

esp_err_t gw_model_deinit_topology(void)
{
    free(s_endpoint_next_slot);
    free(s_endpoint_owners);
    s_endpoint_next_slot = NULL;
    s_endpoint_owners = NULL;
    s_endpoint_owner_cap = 0;
    (void)micro_db_table_deinit(&s_endpoint_table);
    (void)micro_db_table_deinit(&s_device_table);
    return ESP_OK;
}

esp_err_t gw_model_upsert_device(const gw_proto_device_v1_t *record,
                                 bool *out_changed,
                                 bool *out_inserted)
{
    bool changed = false;
    bool inserted = false;
    esp_err_t err = micro_db_table_upsert(&s_device_table,
                                          record,
                                          out_changed ? out_changed : &changed,
                                          out_inserted ? out_inserted : &inserted);
    if (err != ESP_OK) {
        return err;
    }

    if ((out_changed ? *out_changed : changed) || (out_inserted ? *out_inserted : inserted)) {
        (void)gw_model_notify_device_upsert(record);
    }
    return ESP_OK;
}

esp_err_t gw_model_get_device(const gw_device_uid_t *uid,
                              gw_proto_device_v1_t *out_record)
{
    return micro_db_table_get(&s_device_table, uid, out_record);
}

esp_err_t gw_model_remove_device(const gw_device_uid_t *uid,
                                 bool *out_removed)
{
    if (!uid) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t cap = gw_model_count_endpoints();
    if (cap > 0) {
        gw_model_endpoint_key_t *keys = (gw_model_endpoint_key_t *)calloc(cap, sizeof(gw_model_endpoint_key_t));
        if (keys) {
            endpoint_key_list_t list = {
                .keys = keys,
                .count = 0,
                .cap = cap,
            };
            (void)gw_model_iter_endpoints_for_device(uid, collect_endpoint_key_cb, &list);
            for (size_t i = 0; i < list.count; ++i) {
                bool removed = false;
                (void)gw_model_remove_endpoint(&list.keys[i], &removed);
            }
            free(keys);
        }
    }

    bool removed = false;
    esp_err_t err = micro_db_table_remove(&s_device_table, uid, out_removed ? out_removed : &removed);
    if (err == ESP_OK && (out_removed ? *out_removed : removed)) {
        (void)gw_model_notify_device_remove(uid);
    }
    return err;
}

size_t gw_model_count_devices(void)
{
    return micro_db_table_count(&s_device_table);
}

esp_err_t gw_model_get_device_by_index(size_t index,
                                       gw_proto_device_v1_t *out_record)
{
    return micro_db_table_get_by_index(&s_device_table, index, out_record);
}

size_t gw_model_iter_devices(micro_db_iter_cb_t cb, void *user_ctx)
{
    return micro_db_table_iter(&s_device_table, cb, user_ctx);
}

esp_err_t gw_model_upsert_endpoint(const gw_proto_endpoint_v1_t *record,
                                   bool *out_changed,
                                   bool *out_inserted)
{
    bool changed = false;
    bool inserted = false;
    esp_err_t err = micro_db_table_upsert(&s_endpoint_table,
                                          record,
                                          out_changed ? out_changed : &changed,
                                          out_inserted ? out_inserted : &inserted);
    if (err != ESP_OK) {
        return err;
    }
    if ((out_inserted ? *out_inserted : inserted)) {
        uint32_t slot = 0;
        gw_model_endpoint_key_t key = {.uid = record->uid, .endpoint = record->endpoint};
        if (micro_db_table_get_slot(&s_endpoint_table, &key, &slot) == ESP_OK) {
            endpoint_owner_attach(&record->uid, slot);
        }
    }
    if ((out_changed ? *out_changed : changed) || (out_inserted ? *out_inserted : inserted)) {
        (void)gw_model_notify_endpoint_upsert(record);
    }
    return ESP_OK;
}

esp_err_t gw_model_get_endpoint(const gw_model_endpoint_key_t *key,
                                gw_proto_endpoint_v1_t *out_record)
{
    return micro_db_table_get(&s_endpoint_table, key, out_record);
}

esp_err_t gw_model_remove_endpoint(const gw_model_endpoint_key_t *key,
                                   bool *out_removed)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    remove_state_for_endpoint(key);

    uint32_t slot = 0;
    gw_proto_endpoint_v1_t record = {0};
    const bool have_slot = (micro_db_table_get_slot(&s_endpoint_table, key, &slot) == ESP_OK);
    if (have_slot) {
        (void)micro_db_table_get_by_slot(&s_endpoint_table, slot, &record);
    }
    bool removed = false;
    esp_err_t err = micro_db_table_remove(&s_endpoint_table, key, out_removed ? out_removed : &removed);
    if (err == ESP_OK && have_slot && (out_removed ? *out_removed : removed)) {
        endpoint_owner_detach(&record.uid, slot);
        (void)gw_model_notify_endpoint_remove(key, record.short_addr);
    }
    return err;
}

size_t gw_model_count_endpoints(void)
{
    return micro_db_table_count(&s_endpoint_table);
}

esp_err_t gw_model_get_endpoint_by_index(size_t index,
                                         gw_proto_endpoint_v1_t *out_record)
{
    return micro_db_table_get_by_index(&s_endpoint_table, index, out_record);
}

size_t gw_model_iter_endpoints(micro_db_iter_cb_t cb, void *user_ctx)
{
    return micro_db_table_iter(&s_endpoint_table, cb, user_ctx);
}

size_t gw_model_iter_endpoints_for_device(const gw_device_uid_t *uid,
                                          micro_db_iter_cb_t cb,
                                          void *user_ctx)
{
    if (!uid || !cb) {
        return 0;
    }

    bool found = false;
    const int pos = endpoint_owner_find(uid, &found);
    if (!found || pos < 0) {
        return 0;
    }

    size_t visited = 0;
    uint32_t slot = s_endpoint_owners[pos].head_slot;
    while (slot != UINT32_MAX) {
        gw_proto_endpoint_v1_t record = {0};
        if (micro_db_table_get_by_slot(&s_endpoint_table, slot, &record) == ESP_OK) {
            visited++;
            if (!cb(&record, user_ctx)) {
                break;
            }
        }
        slot = s_endpoint_next_slot[slot];
    }

    return visited;
}

bool gw_model_find_device_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid)
{
    if (!out_uid || short_addr == 0) {
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
