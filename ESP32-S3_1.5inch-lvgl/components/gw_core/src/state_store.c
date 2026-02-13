#include "gw_core/state_store.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "gw_state_store";

static bool s_inited;
static gw_state_item_t *s_items;
static size_t s_item_count;
static size_t s_item_cap;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool uid_equals(const gw_device_uid_t *a, const gw_device_uid_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a->uid, b->uid, sizeof(a->uid)) == 0;
}

static bool key_equals(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a, b, GW_STATE_KEY_MAX) == 0;
}

static size_t find_idx_locked(const gw_device_uid_t *uid, uint8_t endpoint, const char *key)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return (size_t)-1;
    }
    for (size_t i = 0; i < s_item_count; i++) {
        if (uid_equals(&s_items[i].uid, uid) &&
            s_items[i].endpoint == endpoint &&
            key_equals(s_items[i].key, key)) {
            return i;
        }
    }
    return (size_t)-1;
}

static size_t find_oldest_idx_locked(void)
{
    if (s_item_count == 0) {
        return (size_t)-1;
    }
    size_t oldest = 0;
    uint64_t oldest_ts = s_items[0].ts_ms;
    for (size_t i = 1; i < s_item_count; i++) {
        if (s_items[i].ts_ms < oldest_ts) {
            oldest = i;
            oldest_ts = s_items[i].ts_ms;
        }
    }
    return oldest;
}

static void state_value_to_str(const gw_state_item_t *item, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    if (!item) {
        strlcpy(out, "null", out_size);
        return;
    }
    switch (item->value_type) {
        case GW_STATE_VALUE_BOOL:
            (void)snprintf(out, out_size, "bool:%d", item->value_bool ? 1 : 0);
            break;
        case GW_STATE_VALUE_F32:
            (void)snprintf(out, out_size, "f32:%.3f", (double)item->value_f32);
            break;
        case GW_STATE_VALUE_U32:
            (void)snprintf(out, out_size, "u32:%u", (unsigned)item->value_u32);
            break;
        case GW_STATE_VALUE_U64:
            (void)snprintf(out, out_size, "u64:%llu", (unsigned long long)item->value_u64);
            break;
        default:
            strlcpy(out, "unknown", out_size);
            break;
    }
}

esp_err_t gw_state_store_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_items == NULL) {
        s_items = (gw_state_item_t *)heap_caps_calloc(GW_STATE_MAX_ITEMS, sizeof(gw_state_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_items) {
            s_items = (gw_state_item_t *)heap_caps_calloc(GW_STATE_MAX_ITEMS, sizeof(gw_state_item_t), MALLOC_CAP_8BIT);
        }
        if (!s_items) {
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGE(TAG, "alloc failed for %u items", (unsigned)GW_STATE_MAX_ITEMS);
            return ESP_ERR_NO_MEM;
        }
        s_item_cap = GW_STATE_MAX_ITEMS;
    }

    s_inited = true;
    s_item_count = 0;
    memset(s_items, 0, s_item_cap * sizeof(gw_state_item_t));
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "initialized cap=%u", (unsigned)s_item_cap);
    return ESP_OK;
}

static esp_err_t upsert_item(const gw_state_item_t *item)
{
    if (!s_inited || item == NULL || item->uid.uid[0] == '\0' || item->key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    enum {
        OP_NONE = 0,
        OP_UPDATE = 1,
        OP_INSERT = 2,
        OP_EVICT = 3,
    } op = OP_NONE;
    gw_state_item_t evicted = {0};
    bool has_evicted = false;
    size_t count_after = 0;

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(&item->uid, item->endpoint, item->key);
    if (idx != (size_t)-1) {
        s_items[idx] = *item;
        op = OP_UPDATE;
        count_after = s_item_count;
        portEXIT_CRITICAL(&s_lock);
        goto log_and_return;
    }

    if (s_item_count < s_item_cap) {
        s_items[s_item_count++] = *item;
        op = OP_INSERT;
        count_after = s_item_count;
        portEXIT_CRITICAL(&s_lock);
        goto log_and_return;
    }

    idx = find_oldest_idx_locked();
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    evicted = s_items[idx];
    has_evicted = true;
    s_items[idx] = *item;
    op = OP_EVICT;
    count_after = s_item_count;
    portEXIT_CRITICAL(&s_lock);

log_and_return:
    if (op == OP_UPDATE) {
        char vbuf[32];
        state_value_to_str(item, vbuf, sizeof(vbuf));
        ESP_LOGI(TAG, "state update uid=%s ep=%u key=%s value=%s ts=%llu",
                 item->uid.uid, (unsigned)item->endpoint, item->key, vbuf, (unsigned long long)item->ts_ms);
    } else if (op == OP_INSERT) {
        char vbuf[32];
        state_value_to_str(item, vbuf, sizeof(vbuf));
        ESP_LOGI(TAG, "state insert uid=%s ep=%u key=%s value=%s ts=%llu items=%u/%u",
                 item->uid.uid, (unsigned)item->endpoint, item->key, vbuf,
                 (unsigned long long)item->ts_ms, (unsigned)count_after, (unsigned)s_item_cap);
    } else if (op == OP_EVICT) {
        char old_v[32];
        char new_v[32];
        state_value_to_str(&evicted, old_v, sizeof(old_v));
        state_value_to_str(item, new_v, sizeof(new_v));
        ESP_LOGW(TAG,
                 "state evict old_uid=%s old_ep=%u old_key=%s old_value=%s -> new_uid=%s new_ep=%u new_key=%s new_value=%s ts=%llu",
                 has_evicted ? evicted.uid.uid : "",
                 has_evicted ? (unsigned)evicted.endpoint : 0u,
                 has_evicted ? evicted.key : "",
                 old_v,
                 item->uid.uid,
                 (unsigned)item->endpoint,
                 item->key,
                 new_v,
                 (unsigned long long)item->ts_ms);
    }

    return ESP_OK;
}

esp_err_t gw_state_store_set_bool(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, bool value, uint64_t ts_ms)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_state_item_t item = {0};
    item.uid = *uid;
    item.endpoint = endpoint;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_type = GW_STATE_VALUE_BOOL;
    item.value_bool = value;
    item.ts_ms = ts_ms;
    return upsert_item(&item);
}

esp_err_t gw_state_store_set_f32(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, float value, uint64_t ts_ms)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_state_item_t item = {0};
    item.uid = *uid;
    item.endpoint = endpoint;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_type = GW_STATE_VALUE_F32;
    item.value_f32 = value;
    item.ts_ms = ts_ms;
    return upsert_item(&item);
}

esp_err_t gw_state_store_set_u32(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, uint32_t value, uint64_t ts_ms)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_state_item_t item = {0};
    item.uid = *uid;
    item.endpoint = endpoint;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_type = GW_STATE_VALUE_U32;
    item.value_u32 = value;
    item.ts_ms = ts_ms;
    return upsert_item(&item);
}

esp_err_t gw_state_store_set_u64(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, uint64_t value, uint64_t ts_ms)
{
    if (uid == NULL || key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    gw_state_item_t item = {0};
    item.uid = *uid;
    item.endpoint = endpoint;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_type = GW_STATE_VALUE_U64;
    item.value_u64 = value;
    item.ts_ms = ts_ms;
    return upsert_item(&item);
}

esp_err_t gw_state_store_get(const gw_device_uid_t *uid, uint8_t endpoint, const char *key, gw_state_item_t *out)
{
    if (!s_inited || uid == NULL || key == NULL || key[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    size_t idx = find_idx_locked(uid, endpoint, key);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_items[idx];
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gw_state_store_get_any(const gw_device_uid_t *uid, const char *key, gw_state_item_t *out)
{
    if (!s_inited || uid == NULL || key == NULL || key[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool found = false;
    gw_state_item_t best = {0};

    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < s_item_count; i++) {
        if (!uid_equals(&s_items[i].uid, uid)) {
            continue;
        }
        if (!key_equals(s_items[i].key, key)) {
            continue;
        }
        if (!found || s_items[i].ts_ms > best.ts_ms) {
            best = s_items[i];
            found = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = best;
    return ESP_OK;
}

size_t gw_state_store_list(const gw_device_uid_t *uid, uint8_t endpoint, gw_state_item_t *out, size_t max_out)
{
    if (!s_inited || uid == NULL || out == NULL || max_out == 0) {
        return 0;
    }

    size_t written = 0;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < s_item_count && written < max_out; i++) {
        if (!uid_equals(&s_items[i].uid, uid)) {
            continue;
        }
        if (s_items[i].endpoint != endpoint) {
            continue;
        }
        out[written++] = s_items[i];
    }
    portEXIT_CRITICAL(&s_lock);
    return written;
}

size_t gw_state_store_list_uid(const gw_device_uid_t *uid, gw_state_item_t *out, size_t max_out)
{
    if (!s_inited || uid == NULL || out == NULL || max_out == 0) {
        return 0;
    }

    size_t written = 0;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < s_item_count && written < max_out; i++) {
        if (uid_equals(&s_items[i].uid, uid)) {
            out[written++] = s_items[i];
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return written;
}
