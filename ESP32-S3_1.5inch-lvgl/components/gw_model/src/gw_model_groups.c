#include "gw_model/gw_model_groups.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_timer.h"
#include "gw_model_notify.h"

static micro_db_table_t s_group_table;
static micro_db_table_t s_group_item_table;

typedef struct {
    char group_id[GW_GROUP_ID_MAX];
    uint32_t head_slot;
    uint8_t state;
} group_item_owner_bucket_t;

static group_item_owner_bucket_t *s_group_item_owners;
static uint32_t *s_group_item_next_slot;
static size_t s_group_item_owner_cap;

static void group_item_owner_attach(const char *group_id, uint32_t slot);

static void normalize_endpoint_key(const gw_model_endpoint_key_t *in, gw_model_endpoint_key_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!in) {
        return;
    }
    out->uid = in->uid;
    out->endpoint = in->endpoint;
}

enum {
    GROUP_OWNER_EMPTY = 0,
    GROUP_OWNER_USED = 1,
    GROUP_OWNER_TOMBSTONE = 2,
};

static void fill_group_key(const char *group_id, gw_model_group_key_t *out_key)
{
    memset(out_key, 0, sizeof(*out_key));
    if (group_id) {
        strlcpy(out_key->id, group_id, sizeof(out_key->id));
    }
}

static uint32_t hash_group_id(const char *group_id)
{
    const uint8_t *p = (const uint8_t *)group_id;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < GW_GROUP_ID_MAX; ++i) {
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

static void rebuild_group_item_owner_index(void)
{
    if (!s_group_item_owners || !s_group_item_next_slot) {
        return;
    }

    memset(s_group_item_owners, 0, s_group_item_owner_cap * sizeof(*s_group_item_owners));
    for (size_t i = 0; i < GW_MODEL_SCHEMA_GROUP_ITEM.max_records; ++i) {
        s_group_item_next_slot[i] = UINT32_MAX;
    }

    for (size_t i = 0; i < micro_db_table_count(&s_group_item_table); ++i) {
        gw_proto_group_item_v1_t record = {0};
        if (micro_db_table_get_by_index(&s_group_item_table, i, &record) != ESP_OK) {
            continue;
        }
        gw_model_endpoint_key_t key = {0};
        key.uid = record.device_uid;
        key.endpoint = record.endpoint;
        uint32_t slot = 0;
        if (micro_db_table_get_slot(&s_group_item_table, &key, &slot) == ESP_OK) {
            group_item_owner_attach(record.group_id, slot);
        }
    }
}

static int group_item_owner_find(const char *group_id, bool *out_found)
{
    const size_t mask = s_group_item_owner_cap - 1u;
    const uint32_t hash = hash_group_id(group_id);
    int first_tombstone = -1;

    for (size_t probe = 0; probe < s_group_item_owner_cap; ++probe) {
        const size_t pos = (hash + probe) & mask;
        const uint8_t state = s_group_item_owners[pos].state;
        if (state == GROUP_OWNER_EMPTY) {
            *out_found = false;
            return (first_tombstone >= 0) ? first_tombstone : (int)pos;
        }
        if (state == GROUP_OWNER_TOMBSTONE) {
            if (first_tombstone < 0) {
                first_tombstone = (int)pos;
            }
            continue;
        }
        if (memcmp(s_group_item_owners[pos].group_id, group_id, GW_GROUP_ID_MAX) == 0) {
            *out_found = true;
            return (int)pos;
        }
    }

    *out_found = false;
    return first_tombstone;
}

static void group_item_owner_attach(const char *group_id, uint32_t slot)
{
    bool found = false;
    const int pos = group_item_owner_find(group_id, &found);
    if (pos < 0) {
        return;
    }

    if (!found) {
        memcpy(s_group_item_owners[pos].group_id, group_id, GW_GROUP_ID_MAX);
        s_group_item_owners[pos].head_slot = UINT32_MAX;
        s_group_item_owners[pos].state = GROUP_OWNER_USED;
    }

    s_group_item_next_slot[slot] = s_group_item_owners[pos].head_slot;
    s_group_item_owners[pos].head_slot = slot;
}

static void group_item_owner_detach(const char *group_id, uint32_t slot)
{
    bool found = false;
    const int pos = group_item_owner_find(group_id, &found);
    if (!found || pos < 0) {
        return;
    }

    uint32_t current = s_group_item_owners[pos].head_slot;
    uint32_t prev = UINT32_MAX;
    while (current != UINT32_MAX) {
        if (current == slot) {
            const uint32_t next = s_group_item_next_slot[current];
            if (prev == UINT32_MAX) {
                s_group_item_owners[pos].head_slot = next;
            } else {
                s_group_item_next_slot[prev] = next;
            }
            s_group_item_next_slot[current] = UINT32_MAX;
            if (s_group_item_owners[pos].head_slot == UINT32_MAX) {
                s_group_item_owners[pos].state = GROUP_OWNER_TOMBSTONE;
            }
            return;
        }
        prev = current;
        current = s_group_item_next_slot[current];
    }
}

static int compare_group_items(const gw_proto_group_item_v1_t *lhs,
                               const gw_proto_group_item_v1_t *rhs)
{
    if (!lhs || !rhs) {
        return 0;
    }

    if (lhs->order < rhs->order) return -1;
    if (lhs->order > rhs->order) return 1;

    const int uid_cmp = strcasecmp(lhs->device_uid.uid, rhs->device_uid.uid);
    if (uid_cmp != 0) return uid_cmp;

    if (lhs->endpoint < rhs->endpoint) return -1;
    if (lhs->endpoint > rhs->endpoint) return 1;
    return 0;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t touch_group_version(const char *group_id)
{
    gw_proto_group_v1_t group = {0};
    esp_err_t err = gw_model_get_group(group_id, &group);
    if (err != ESP_OK) {
        return err;
    }

    group.version += 1u;
    group.updated_at_ms = now_ms();
    return gw_model_upsert_group(&group, NULL, NULL);
}

esp_err_t gw_model_init_groups(void)
{
    esp_err_t err = micro_db_table_init(&s_group_table, &GW_MODEL_SCHEMA_GROUP);
    if (err != ESP_OK) {
        return err;
    }

    err = micro_db_table_init(&s_group_item_table, &GW_MODEL_SCHEMA_GROUP_ITEM);
    if (err != ESP_OK) {
        (void)micro_db_table_deinit(&s_group_table);
        return err;
    }

    s_group_item_owner_cap = next_owner_capacity(GW_MODEL_SCHEMA_GROUP_ITEM.max_records);
    s_group_item_owners = (group_item_owner_bucket_t *)calloc(s_group_item_owner_cap, sizeof(group_item_owner_bucket_t));
    s_group_item_next_slot = (uint32_t *)calloc(GW_MODEL_SCHEMA_GROUP_ITEM.max_records, sizeof(uint32_t));
    if (!s_group_item_owners || !s_group_item_next_slot) {
        free(s_group_item_next_slot);
        free(s_group_item_owners);
        s_group_item_next_slot = NULL;
        s_group_item_owners = NULL;
        (void)micro_db_table_deinit(&s_group_item_table);
        (void)micro_db_table_deinit(&s_group_table);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < GW_MODEL_SCHEMA_GROUP_ITEM.max_records; ++i) {
        s_group_item_next_slot[i] = UINT32_MAX;
    }

    rebuild_group_item_owner_index();

    return ESP_OK;
}

esp_err_t gw_model_deinit_groups(void)
{
    free(s_group_item_next_slot);
    free(s_group_item_owners);
    s_group_item_next_slot = NULL;
    s_group_item_owners = NULL;
    s_group_item_owner_cap = 0;
    (void)micro_db_table_deinit(&s_group_item_table);
    (void)micro_db_table_deinit(&s_group_table);
    return ESP_OK;
}

esp_err_t gw_model_upsert_group(const gw_proto_group_v1_t *record,
                                bool *out_changed,
                                bool *out_inserted)
{
    bool changed = false;
    bool inserted = false;
    esp_err_t err = micro_db_table_upsert(&s_group_table,
                                          record,
                                          out_changed ? out_changed : &changed,
                                          out_inserted ? out_inserted : &inserted);
    if (err != ESP_OK) {
        return err;
    }
    if ((out_changed ? *out_changed : changed) || (out_inserted ? *out_inserted : inserted)) {
        (void)gw_model_notify_group_upsert(record);
    }
    return ESP_OK;
}

esp_err_t gw_model_get_group(const char *group_id,
                             gw_proto_group_v1_t *out_record)
{
    gw_model_group_key_t key = {0};
    fill_group_key(group_id, &key);
    return micro_db_table_get(&s_group_table, &key, out_record);
}

esp_err_t gw_model_remove_group(const char *group_id,
                                bool *out_removed)
{
    gw_model_group_key_t key = {0};
    fill_group_key(group_id, &key);
    bool found = false;
    int pos = group_item_owner_find(key.id, &found);
    while (found && pos >= 0 && s_group_item_owners[pos].head_slot != UINT32_MAX) {
        gw_proto_group_item_v1_t item = {0};
        if (micro_db_table_get_by_slot(&s_group_item_table, s_group_item_owners[pos].head_slot, &item) != ESP_OK) {
            break;
        }

        gw_model_endpoint_key_t item_key = {0};
        item_key.uid = item.device_uid;
        item_key.endpoint = item.endpoint;
        bool item_removed = false;
        (void)gw_model_remove_group_item(&item_key, &item_removed);
        if (!item_removed) {
            break;
        }
        found = false;
        pos = group_item_owner_find(key.id, &found);
    }

    bool removed = false;
    esp_err_t err = micro_db_table_remove(&s_group_table, &key, out_removed ? out_removed : &removed);
    if (err == ESP_OK && (out_removed ? *out_removed : removed)) {
        (void)gw_model_notify_group_remove(group_id);
    }
    return err;
}

size_t gw_model_count_groups(void)
{
    return micro_db_table_count(&s_group_table);
}

esp_err_t gw_model_get_group_by_index(size_t index,
                                      gw_proto_group_v1_t *out_record)
{
    return micro_db_table_get_by_index(&s_group_table, index, out_record);
}

size_t gw_model_iter_groups(micro_db_iter_cb_t cb, void *user_ctx)
{
    return micro_db_table_iter(&s_group_table, cb, user_ctx);
}

esp_err_t gw_model_upsert_group_item(const gw_proto_group_item_v1_t *record,
                                     bool *out_changed,
                                     bool *out_inserted)
{
    gw_model_endpoint_key_t key = {0};
    key.uid = record->device_uid;
    key.endpoint = record->endpoint;
    gw_proto_group_item_v1_t existing = {0};
    uint32_t existing_slot = 0;
    const bool had_existing =
        (micro_db_table_get_slot(&s_group_item_table, &key, &existing_slot) == ESP_OK) &&
        (micro_db_table_get_by_slot(&s_group_item_table, existing_slot, &existing) == ESP_OK);

    bool changed = false;
    bool inserted = false;
    esp_err_t err = micro_db_table_upsert(&s_group_item_table,
                                          record,
                                          out_changed ? out_changed : &changed,
                                          out_inserted ? out_inserted : &inserted);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t slot = 0;
    if (micro_db_table_get_slot(&s_group_item_table, &key, &slot) != ESP_OK) {
        return ESP_OK;
    }

    if (had_existing && memcmp(existing.group_id, record->group_id, sizeof(existing.group_id)) != 0) {
        group_item_owner_detach(existing.group_id, existing_slot);
        group_item_owner_attach(record->group_id, slot);
        (void)gw_model_notify_group_item_upsert(record);
        return ESP_OK;
    }

    if ((out_inserted ? *out_inserted : inserted)) {
        group_item_owner_attach(record->group_id, slot);
    }

    if ((out_changed ? *out_changed : changed) || (out_inserted ? *out_inserted : inserted)) {
        (void)gw_model_notify_group_item_upsert(record);
    }

    return ESP_OK;
}

esp_err_t gw_model_get_group_item(const gw_model_endpoint_key_t *key,
                                  gw_proto_group_item_v1_t *out_record)
{
    gw_model_endpoint_key_t normalized = {0};
    normalize_endpoint_key(key, &normalized);
    return micro_db_table_get(&s_group_item_table, &normalized, out_record);
}

esp_err_t gw_model_remove_group_item(const gw_model_endpoint_key_t *key,
                                     bool *out_removed)
{
    gw_model_endpoint_key_t normalized = {0};
    normalize_endpoint_key(key, &normalized);
    uint32_t slot = 0;
    gw_proto_group_item_v1_t record = {0};
    const bool have_slot = (micro_db_table_get_slot(&s_group_item_table, &normalized, &slot) == ESP_OK);
    if (have_slot) {
        (void)micro_db_table_get_by_slot(&s_group_item_table, slot, &record);
    }

    bool removed = false;
    esp_err_t err = micro_db_table_remove(&s_group_item_table, &normalized, out_removed ? out_removed : &removed);
    if (err == ESP_OK && have_slot && (out_removed ? *out_removed : removed)) {
        group_item_owner_detach(record.group_id, slot);
        (void)gw_model_notify_group_item_remove(&normalized);
    }
    return err;
}

size_t gw_model_count_group_items(void)
{
    return micro_db_table_count(&s_group_item_table);
}

esp_err_t gw_model_get_group_item_by_index(size_t index,
                                           gw_proto_group_item_v1_t *out_record)
{
    return micro_db_table_get_by_index(&s_group_item_table, index, out_record);
}

size_t gw_model_iter_group_items(micro_db_iter_cb_t cb, void *user_ctx)
{
    return micro_db_table_iter(&s_group_item_table, cb, user_ctx);
}

size_t gw_model_count_group_items_for_group(const char *group_id)
{
    if (!group_id || !group_id[0]) {
        return 0;
    }

    bool found = false;
    const int pos = group_item_owner_find(group_id, &found);
    if (!found || pos < 0) {
        return 0;
    }

    size_t count = 0;
    uint32_t slot = s_group_item_owners[pos].head_slot;
    while (slot != UINT32_MAX) {
        gw_proto_group_item_v1_t record = {0};
        if (micro_db_table_get_by_slot(&s_group_item_table, slot, &record) == ESP_OK) {
            count++;
        }
        slot = s_group_item_next_slot[slot];
    }
    return count;
}

size_t gw_model_iter_group_items_for_group(const char *group_id,
                                           micro_db_iter_cb_t cb,
                                           void *user_ctx)
{
    if (!group_id || !group_id[0] || !cb) {
        return 0;
    }

    bool found = false;
    const int pos = group_item_owner_find(group_id, &found);
    if (!found || pos < 0) {
        return 0;
    }

    size_t visited = 0;
    uint32_t slot = s_group_item_owners[pos].head_slot;
    while (slot != UINT32_MAX) {
        gw_proto_group_item_v1_t record = {0};
        if (micro_db_table_get_by_slot(&s_group_item_table, slot, &record) == ESP_OK) {
            visited++;
            if (!cb(&record, user_ctx)) {
                break;
            }
        }
        slot = s_group_item_next_slot[slot];
    }

    return visited;
}

esp_err_t gw_model_get_group_item_for_group_by_index(const char *group_id,
                                                     size_t index,
                                                     gw_proto_group_item_v1_t *out_record)
{
    if (!group_id || !group_id[0] || !out_record) {
        return ESP_ERR_INVALID_ARG;
    }

    bool found = false;
    const int pos = group_item_owner_find(group_id, &found);
    if (!found || pos < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t selected_slot = UINT32_MAX;
    gw_proto_group_item_v1_t selected = {0};

    uint32_t slot = s_group_item_owners[pos].head_slot;
    while (slot != UINT32_MAX) {
        gw_proto_group_item_v1_t candidate = {0};
        if (micro_db_table_get_by_slot(&s_group_item_table, slot, &candidate) == ESP_OK) {
            size_t rank = 0;
            uint32_t other_slot = s_group_item_owners[pos].head_slot;
            while (other_slot != UINT32_MAX) {
                gw_proto_group_item_v1_t other = {0};
                if (micro_db_table_get_by_slot(&s_group_item_table, other_slot, &other) == ESP_OK &&
                    compare_group_items(&other, &candidate) < 0) {
                    rank++;
                }
                other_slot = s_group_item_next_slot[other_slot];
            }

            if (rank == index) {
                selected_slot = slot;
                selected = candidate;
                break;
            }
        }
        slot = s_group_item_next_slot[slot];
    }

    if (selected_slot == UINT32_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_record = selected;
    return ESP_OK;
}

esp_err_t gw_model_create_group(const char *id_opt,
                                const char *name,
                                gw_proto_group_v1_t *out_created)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_group_v1_t group = {0};
    if (id_opt && id_opt[0]) {
        strlcpy(group.id, id_opt, sizeof(group.id));
    } else {
        snprintf(group.id, sizeof(group.id), "grp_%" PRIu32, now_ms());
    }
    if (!group.id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (gw_model_get_group(group.id, &group) == ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&group, 0, sizeof(group));
    if (id_opt && id_opt[0]) {
        strlcpy(group.id, id_opt, sizeof(group.id));
    } else {
        snprintf(group.id, sizeof(group.id), "grp_%" PRIu32, now_ms());
    }
    strlcpy(group.name, name, sizeof(group.name));
    group.version = 1u;
    group.created_at_ms = now_ms();
    group.updated_at_ms = group.created_at_ms;

    esp_err_t err = gw_model_upsert_group(&group, NULL, NULL);
    if (err == ESP_OK && out_created) {
        *out_created = group;
    }
    return err;
}

esp_err_t gw_model_rename_group(const char *group_id,
                                const char *name)
{
    if (!group_id || !group_id[0] || !name) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_group_v1_t group = {0};
    esp_err_t err = gw_model_get_group(group_id, &group);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(group.name, name, sizeof(group.name));
    group.version += 1u;
    group.updated_at_ms = now_ms();
    return gw_model_upsert_group(&group, NULL, NULL);
}

esp_err_t gw_model_set_group_item(const char *group_id,
                                  const gw_device_uid_t *device_uid,
                                  uint8_t endpoint)
{
    if (!group_id || !group_id[0] || !device_uid || !device_uid->uid[0] || endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_group_v1_t group = {0};
    if (gw_model_get_group(group_id, &group) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    gw_model_endpoint_key_t key = {
        .uid = *device_uid,
        .endpoint = endpoint,
    };
    gw_proto_group_item_v1_t item = {0};
    if (gw_model_get_group_item(&key, &item) != ESP_OK) {
        memset(&item, 0, sizeof(item));
        item.device_uid = *device_uid;
        item.endpoint = endpoint;
        item.version = 1u;
        item.order = now_ms();
    } else {
        item.version += 1u;
        if (strncmp(item.group_id, group_id, sizeof(item.group_id)) != 0) {
            item.order = now_ms();
        }
    }

    strlcpy(item.group_id, group_id, sizeof(item.group_id));
    esp_err_t err = gw_model_upsert_group_item(&item, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return touch_group_version(group_id);
}

esp_err_t gw_model_remove_group_item_by_endpoint(const gw_device_uid_t *device_uid,
                                                 uint8_t endpoint)
{
    if (!device_uid || !device_uid->uid[0] || endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_model_endpoint_key_t key = {
        .uid = *device_uid,
        .endpoint = endpoint,
    };
    gw_proto_group_item_v1_t item = {0};
    esp_err_t err = gw_model_get_group_item(&key, &item);
    if (err != ESP_OK) {
        return err;
    }

    bool removed = false;
    err = gw_model_remove_group_item(&key, &removed);
    if (err != ESP_OK || !removed) {
        return err;
    }
    return touch_group_version(item.group_id);
}

esp_err_t gw_model_reorder_group_item(const char *group_id,
                                      const gw_device_uid_t *device_uid,
                                      uint8_t endpoint,
                                      uint32_t order)
{
    if (!group_id || !group_id[0] || !device_uid || !device_uid->uid[0] || endpoint == 0 || order == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_model_endpoint_key_t key = {
        .uid = *device_uid,
        .endpoint = endpoint,
    };
    gw_proto_group_item_v1_t item = {0};
    esp_err_t err = gw_model_get_group_item(&key, &item);
    if (err != ESP_OK) {
        return err;
    }
    if (strncmp(item.group_id, group_id, sizeof(item.group_id)) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    item.order = order;
    item.version += 1u;
    err = gw_model_upsert_group_item(&item, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return touch_group_version(group_id);
}

esp_err_t gw_model_set_group_item_label(const gw_device_uid_t *device_uid,
                                        uint8_t endpoint,
                                        const char *label)
{
    if (!device_uid || !device_uid->uid[0] || endpoint == 0 || !label) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_model_endpoint_key_t key = {
        .uid = *device_uid,
        .endpoint = endpoint,
    };
    gw_proto_group_item_v1_t item = {0};
    esp_err_t err = gw_model_get_group_item(&key, &item);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(item.label, label, sizeof(item.label));
    item.version += 1u;
    err = gw_model_upsert_group_item(&item, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return touch_group_version(item.group_id);
}
