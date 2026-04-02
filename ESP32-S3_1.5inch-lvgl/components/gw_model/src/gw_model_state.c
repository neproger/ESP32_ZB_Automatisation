#include "gw_model/gw_model_state.h"

#include <stdlib.h>
#include <string.h>

#include "gw_model_notify.h"

static micro_db_table_t s_state_table;

typedef struct {
    gw_model_endpoint_key_t owner;
    uint32_t head_slot;
    uint8_t state;
} state_owner_bucket_t;

static state_owner_bucket_t *s_state_owners;
static uint32_t *s_state_next_slot;
static size_t s_state_owner_cap;

static void state_owner_attach(const gw_model_endpoint_key_t *owner, uint32_t slot);

static void normalize_endpoint_owner_key(const gw_model_endpoint_key_t *in, gw_model_endpoint_key_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!in) {
        return;
    }
    out->uid = in->uid;
    out->endpoint = in->endpoint;
}

static void normalize_state_key(const gw_model_state_key_t *in, gw_model_state_key_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!in) {
        return;
    }
    out->uid = in->uid;
    out->endpoint = in->endpoint;
    memcpy(out->key, in->key, sizeof(out->key));
}

enum {
    STATE_OWNER_EMPTY = 0,
    STATE_OWNER_USED = 1,
    STATE_OWNER_TOMBSTONE = 2,
};

static uint32_t hash_endpoint_owner(const gw_model_endpoint_key_t *owner)
{
    const uint8_t *p = (const uint8_t *)owner;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(*owner); ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static size_t next_state_owner_capacity(size_t max_records)
{
    size_t cap = 1;
    const size_t doubled = max_records * 2u;
    const size_t target = doubled > 0 ? doubled : 2u;
    while (cap < target) {
        cap <<= 1u;
    }
    return cap;
}

static void rebuild_state_owner_index(void)
{
    if (!s_state_owners || !s_state_next_slot) {
        return;
    }

    memset(s_state_owners, 0, s_state_owner_cap * sizeof(*s_state_owners));
    for (size_t i = 0; i < GW_STATE_MAX_ITEMS; ++i) {
        s_state_next_slot[i] = UINT32_MAX;
    }

    for (size_t i = 0; i < micro_db_table_count(&s_state_table); ++i) {
        gw_proto_state_item_v1_t record = {0};
        if (micro_db_table_get_by_index(&s_state_table, i, &record) != ESP_OK) {
            continue;
        }
        gw_model_state_key_t key = {0};
        key.uid = record.uid;
        key.endpoint = record.endpoint;
        memcpy(key.key, record.key, sizeof(key.key));
        uint32_t slot = 0;
        if (micro_db_table_get_slot(&s_state_table, &key, &slot) == ESP_OK) {
            gw_model_endpoint_key_t owner = {0};
            owner.uid = record.uid;
            owner.endpoint = record.endpoint;
            state_owner_attach(&owner, slot);
        }
    }
}

static int state_owner_find(const gw_model_endpoint_key_t *owner, bool *out_found)
{
    const size_t mask = s_state_owner_cap - 1u;
    const uint32_t hash = hash_endpoint_owner(owner);
    int first_tombstone = -1;

    for (size_t probe = 0; probe < s_state_owner_cap; ++probe) {
        const size_t pos = (hash + probe) & mask;
        const uint8_t state = s_state_owners[pos].state;
        if (state == STATE_OWNER_EMPTY) {
            *out_found = false;
            return (first_tombstone >= 0) ? first_tombstone : (int)pos;
        }
        if (state == STATE_OWNER_TOMBSTONE) {
            if (first_tombstone < 0) {
                first_tombstone = (int)pos;
            }
            continue;
        }
        if (memcmp(&s_state_owners[pos].owner, owner, sizeof(*owner)) == 0) {
            *out_found = true;
            return (int)pos;
        }
    }

    *out_found = false;
    return first_tombstone;
}

static void state_owner_attach(const gw_model_endpoint_key_t *owner, uint32_t slot)
{
    bool found = false;
    const int pos = state_owner_find(owner, &found);
    if (pos < 0) {
        return;
    }

    if (!found) {
        s_state_owners[pos].owner = *owner;
        s_state_owners[pos].head_slot = UINT32_MAX;
        s_state_owners[pos].state = STATE_OWNER_USED;
    }

    s_state_next_slot[slot] = s_state_owners[pos].head_slot;
    s_state_owners[pos].head_slot = slot;
}

static void state_owner_detach(const gw_model_endpoint_key_t *owner, uint32_t slot)
{
    bool found = false;
    const int pos = state_owner_find(owner, &found);
    if (!found || pos < 0) {
        return;
    }

    uint32_t current = s_state_owners[pos].head_slot;
    uint32_t prev = UINT32_MAX;
    while (current != UINT32_MAX) {
        if (current == slot) {
            const uint32_t next = s_state_next_slot[current];
            if (prev == UINT32_MAX) {
                s_state_owners[pos].head_slot = next;
            } else {
                s_state_next_slot[prev] = next;
            }
            s_state_next_slot[current] = UINT32_MAX;
            if (s_state_owners[pos].head_slot == UINT32_MAX) {
                s_state_owners[pos].state = STATE_OWNER_TOMBSTONE;
            }
            return;
        }
        prev = current;
        current = s_state_next_slot[current];
    }
}

esp_err_t gw_model_init_state(void)
{
    esp_err_t err = micro_db_table_init(&s_state_table, &GW_MODEL_SCHEMA_STATE_ITEM);
    if (err != ESP_OK) {
        return err;
    }

    s_state_owner_cap = next_state_owner_capacity(GW_STATE_MAX_ITEMS);
    s_state_owners = (state_owner_bucket_t *)calloc(s_state_owner_cap, sizeof(state_owner_bucket_t));
    s_state_next_slot = (uint32_t *)calloc(GW_STATE_MAX_ITEMS, sizeof(uint32_t));
    if (!s_state_owners || !s_state_next_slot) {
        free(s_state_next_slot);
        free(s_state_owners);
        s_state_next_slot = NULL;
        s_state_owners = NULL;
        (void)micro_db_table_deinit(&s_state_table);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < GW_STATE_MAX_ITEMS; ++i) {
        s_state_next_slot[i] = UINT32_MAX;
    }
    rebuild_state_owner_index();
    return ESP_OK;
}

esp_err_t gw_model_deinit_state(void)
{
    free(s_state_next_slot);
    free(s_state_owners);
    s_state_next_slot = NULL;
    s_state_owners = NULL;
    s_state_owner_cap = 0;
    return micro_db_table_deinit(&s_state_table);
}

esp_err_t gw_model_upsert_state(const gw_proto_state_item_v1_t *record,
                                bool *out_changed,
                                bool *out_inserted)
{
    bool changed = false;
    bool inserted = false;
    esp_err_t err = micro_db_table_upsert(&s_state_table,
                                          record,
                                          out_changed ? out_changed : &changed,
                                          out_inserted ? out_inserted : &inserted);
    if (err != ESP_OK) {
        return err;
    }
    if ((out_inserted ? *out_inserted : inserted)) {
        uint32_t slot = 0;
        gw_model_state_key_t key = {0};
        key.uid = record->uid;
        key.endpoint = record->endpoint;
        memcpy(key.key, record->key, sizeof(key.key));
        if (micro_db_table_get_slot(&s_state_table, &key, &slot) == ESP_OK) {
            gw_model_endpoint_key_t owner = {0};
            owner.uid = record->uid;
            owner.endpoint = record->endpoint;
            state_owner_attach(&owner, slot);
        }
    }
    if ((out_changed ? *out_changed : changed) || (out_inserted ? *out_inserted : inserted)) {
        (void)gw_model_notify_state_upsert(record);
    }
    return ESP_OK;
}

esp_err_t gw_model_get_state(const gw_model_state_key_t *key,
                             gw_proto_state_item_v1_t *out_record)
{
    gw_model_state_key_t normalized = {0};
    normalize_state_key(key, &normalized);
    return micro_db_table_get(&s_state_table, &normalized, out_record);
}

esp_err_t gw_model_remove_state(const gw_model_state_key_t *key,
                                bool *out_removed)
{
    gw_model_state_key_t normalized = {0};
    normalize_state_key(key, &normalized);
    uint32_t slot = 0;
    gw_proto_state_item_v1_t record = {0};
    const bool have_slot = (micro_db_table_get_slot(&s_state_table, &normalized, &slot) == ESP_OK);
    if (have_slot) {
        (void)micro_db_table_get_by_slot(&s_state_table, slot, &record);
    }
    bool removed = false;
    esp_err_t err = micro_db_table_remove(&s_state_table, &normalized, out_removed ? out_removed : &removed);
    if (err == ESP_OK && have_slot && (out_removed ? *out_removed : removed)) {
        gw_model_endpoint_key_t owner = {0};
        owner.uid = record.uid;
        owner.endpoint = record.endpoint;
        state_owner_detach(&owner, slot);
        (void)gw_model_notify_state_remove(&normalized);
    }
    return err;
}

size_t gw_model_count_state(void)
{
    return micro_db_table_count(&s_state_table);
}

esp_err_t gw_model_get_state_by_index(size_t index,
                                      gw_proto_state_item_v1_t *out_record)
{
    return micro_db_table_get_by_index(&s_state_table, index, out_record);
}

size_t gw_model_iter_state(micro_db_iter_cb_t cb, void *user_ctx)
{
    return micro_db_table_iter(&s_state_table, cb, user_ctx);
}

size_t gw_model_iter_state_for_endpoint(const gw_model_endpoint_key_t *owner,
                                        micro_db_iter_cb_t cb,
                                        void *user_ctx)
{
    if (!owner || !cb) {
        return 0;
    }

    gw_model_endpoint_key_t normalized = {0};
    normalize_endpoint_owner_key(owner, &normalized);

    bool found = false;
    const int pos = state_owner_find(&normalized, &found);
    if (!found || pos < 0) {
        return 0;
    }

    size_t visited = 0;
    uint32_t slot = s_state_owners[pos].head_slot;
    while (slot != UINT32_MAX) {
        gw_proto_state_item_v1_t record = {0};
        if (micro_db_table_get_by_slot(&s_state_table, slot, &record) == ESP_OK) {
            visited++;
            if (!cb(&record, user_ctx)) {
                break;
            }
        }
        slot = s_state_next_slot[slot];
    }

    return visited;
}

typedef struct {
    const gw_device_uid_t *uid;
    micro_db_iter_cb_t cb;
    void *user_ctx;
    size_t matched;
} state_iter_device_ctx_t;

static bool state_iter_for_device_cb(const void *record, void *user_ctx)
{
    state_iter_device_ctx_t *ctx = (state_iter_device_ctx_t *)user_ctx;
    const gw_proto_state_item_v1_t *st = (const gw_proto_state_item_v1_t *)record;

    if (memcmp(&st->uid, ctx->uid, sizeof(*ctx->uid)) != 0) {
        return true;
    }

    ctx->matched++;
    return ctx->cb(record, ctx->user_ctx);
}

size_t gw_model_iter_state_for_device(const gw_device_uid_t *uid,
                                      micro_db_iter_cb_t cb,
                                      void *user_ctx)
{
    if (!uid || !cb) {
        return 0;
    }

    state_iter_device_ctx_t ctx = {
        .uid = uid,
        .cb = cb,
        .user_ctx = user_ctx,
        .matched = 0,
    };

    (void)micro_db_table_iter(&s_state_table, state_iter_for_device_cb, &ctx);
    return ctx.matched;
}
