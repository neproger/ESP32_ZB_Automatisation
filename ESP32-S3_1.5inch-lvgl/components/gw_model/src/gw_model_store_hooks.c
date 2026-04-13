#include <stdlib.h>
#include <string.h>

#include "gw_model/gw_model_device_meta.h"
#include "gw_model/gw_model_state.h"
#include "gw_model_notify.h"
#include "gw_store/gw_store_hooks.h"

typedef struct {
    gw_model_state_key_t *keys;
    size_t count;
    size_t cap;
} state_key_list_t;

void gw_model_store_hooks_force_link(void)
{
    /* Intentionally empty.
     * Called from gw_model_init() to force this TU into link and override
     * weak default hooks from shared gw_store_hooks_default.c.
     */
}

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

void gw_store_hook_remove_state_for_endpoint(const gw_store_endpoint_key_t *endpoint_key)
{
    if (!endpoint_key) {
        return;
    }

    const gw_model_endpoint_key_t owner = {
        .uid = endpoint_key->uid,
        .endpoint = endpoint_key->endpoint,
    };
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
    (void)gw_model_iter_state_for_endpoint(&owner, collect_state_key_cb, &list);
    for (size_t i = 0; i < list.count; ++i) {
        bool removed = false;
        (void)gw_model_remove_state(&list.keys[i], &removed);
    }

    free(keys);
}

esp_err_t gw_store_hook_notify_device_upsert(const gw_proto_device_v1_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_device_v1_t merged = {0};
    merged = *record;
    gw_model_apply_device_meta(&merged);
    return gw_model_notify_device_upsert(&merged);
}

esp_err_t gw_store_hook_notify_device_remove(const gw_device_uid_t *uid)
{
    return gw_model_notify_device_remove(uid);
}

esp_err_t gw_store_hook_notify_endpoint_upsert(const gw_proto_endpoint_v1_t *record)
{
    return gw_model_notify_endpoint_upsert(record);
}

esp_err_t gw_store_hook_notify_endpoint_remove(const gw_store_endpoint_key_t *key, uint16_t short_addr)
{
    const gw_model_endpoint_key_t endpoint_key = {
        .uid = key->uid,
        .endpoint = key->endpoint,
    };
    return gw_model_notify_endpoint_remove(&endpoint_key, short_addr);
}
