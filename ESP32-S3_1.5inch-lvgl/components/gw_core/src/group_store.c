#include "gw_core/group_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gw_core/storage.h"

static const char *TAG = "gw_groups";

static const uint32_t GROUPS_MAGIC = 0x47525053;      // GRPS
static const uint16_t GROUPS_VERSION = 1;
static const size_t GROUPS_MAX = 24;

static const uint32_t GROUP_ITEMS_MAGIC = 0x47525049; // GRPI
static const uint16_t GROUP_ITEMS_VERSION = 2;
static const size_t GROUP_ITEMS_MAX = 256;

static gw_storage_t s_groups_storage;
static gw_storage_t s_items_storage;
static bool s_initialized = false;
static uint32_t s_version_seq = 0;
#define GW_GROUP_LISTENER_CAP 4
typedef struct {
    gw_group_store_listener_t cb;
    void *user_ctx;
} gw_group_listener_slot_t;
static gw_group_listener_slot_t s_listeners[GW_GROUP_LISTENER_CAP];
static portMUX_TYPE s_listener_lock = portMUX_INITIALIZER_UNLOCKED;

static void notify_group_listeners(void)
{
    gw_group_listener_slot_t listeners[GW_GROUP_LISTENER_CAP];
    size_t listener_count = 0;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_GROUP_LISTENER_CAP; i++) {
        if (s_listeners[i].cb) {
            listeners[listener_count++] = s_listeners[i];
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < listener_count; i++) {
        listeners[i].cb(listeners[i].user_ctx);
    }
}

static const gw_storage_desc_t s_groups_desc = {
    .key = "groups",
    .item_size = sizeof(gw_group_entry_t),
    .max_items = GROUPS_MAX,
    .magic = GROUPS_MAGIC,
    .version = GROUPS_VERSION,
    .namespace = "groups",
};

static const gw_storage_desc_t s_items_desc = {
    .key = "group_items",
    .item_size = sizeof(gw_group_item_t),
    .max_items = GROUP_ITEMS_MAX,
    .magic = GROUP_ITEMS_MAGIC,
    .version = GROUP_ITEMS_VERSION,
    .namespace = "groups",
};

static bool ready(void)
{
    return s_initialized && s_groups_storage.data && s_items_storage.data;
}

static int compare_group_items(const gw_group_item_t *lhs, const gw_group_item_t *rhs)
{
    if (!lhs || !rhs) return 0;

    const int group_cmp = strncmp(lhs->group_id, rhs->group_id, sizeof(lhs->group_id));
    if (group_cmp != 0) return group_cmp;

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

static uint32_t next_version(void)
{
    return ++s_version_seq;
}

static bool uid_equals(const gw_device_uid_t *a, const gw_device_uid_t *b)
{
    if (!a || !b) return false;
    return strcasecmp(a->uid, b->uid) == 0;
}

static size_t find_group_idx(const char *id)
{
    if (!id || !ready()) return (size_t)-1;
    gw_group_entry_t *groups = (gw_group_entry_t *)s_groups_storage.data;
    for (size_t i = 0; i < s_groups_storage.count; i++) {
        if (strncmp(groups[i].id, id, sizeof(groups[i].id)) == 0) return i;
    }
    return (size_t)-1;
}

static bool group_exists(const char *id)
{
    return find_group_idx(id) != (size_t)-1;
}

static bool touch_group_version(const char *group_id, uint32_t updated_at_ms)
{
    if (!group_id || !ready()) return false;

    bool touched = false;
    portENTER_CRITICAL(&s_groups_storage.lock);
    size_t idx = find_group_idx(group_id);
    if (idx != (size_t)-1) {
        gw_group_entry_t *groups = (gw_group_entry_t *)s_groups_storage.data;
        groups[idx].version = next_version();
        groups[idx].updated_at_ms = updated_at_ms;
        touched = true;
    }
    portEXIT_CRITICAL(&s_groups_storage.lock);
    return touched;
}

static esp_err_t persist_all(void)
{
    esp_err_t err = gw_storage_save(&s_groups_storage);
    if (err != ESP_OK) return err;
    return gw_storage_save(&s_items_storage);
}

esp_err_t gw_group_store_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = gw_storage_init(&s_groups_storage, &s_groups_desc, GW_STORAGE_NVS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "groups storage init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = gw_storage_init(&s_items_storage, &s_items_desc, GW_STORAGE_NVS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "group items storage init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Group storage initialized: groups=%u items=%u",
             (unsigned)s_groups_storage.count,
             (unsigned)s_items_storage.count);
    return ESP_OK;
}

esp_err_t gw_group_store_add_listener(gw_group_store_listener_t cb, void *user_ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_GROUP_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < GW_GROUP_LISTENER_CAP; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i].cb = cb;
            s_listeners[i].user_ctx = user_ctx;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t gw_group_store_remove_listener(gw_group_store_listener_t cb, void *user_ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_GROUP_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            s_listeners[i].cb = NULL;
            s_listeners[i].user_ctx = NULL;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NOT_FOUND;
}

size_t gw_group_store_count(void)
{
    if (!ready()) return 0;
    portENTER_CRITICAL(&s_groups_storage.lock);
    const size_t count = s_groups_storage.count;
    portEXIT_CRITICAL(&s_groups_storage.lock);
    return count;
}

esp_err_t gw_group_store_get_by_index(size_t index, gw_group_entry_t *out_group)
{
    if (!ready() || !out_group) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&s_groups_storage.lock);
    if (index < s_groups_storage.count) {
        gw_group_entry_t *groups = (gw_group_entry_t *)s_groups_storage.data;
        *out_group = groups[index];
        err = ESP_OK;
    }
    portEXIT_CRITICAL(&s_groups_storage.lock);
    return err;
}

size_t gw_group_store_get_group_item_count(const char *group_id)
{
    if (!ready() || !group_id) return 0;

    size_t count = 0;
    portENTER_CRITICAL(&s_items_storage.lock);
    gw_group_item_t *items = (gw_group_item_t *)s_items_storage.data;
    for (size_t i = 0; i < s_items_storage.count; i++) {
        if (strncmp(items[i].group_id, group_id, sizeof(items[i].group_id)) == 0) {
            count++;
        }
    }
    portEXIT_CRITICAL(&s_items_storage.lock);
    return count;
}

esp_err_t gw_group_store_get_group_item_by_index(const char *group_id, size_t index, gw_group_item_t *out_item)
{
    if (!ready() || !group_id || !out_item) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&s_items_storage.lock);
    gw_group_item_t *items = (gw_group_item_t *)s_items_storage.data;
    for (size_t i = 0; i < s_items_storage.count; i++) {
        if (strncmp(items[i].group_id, group_id, sizeof(items[i].group_id)) != 0) {
            continue;
        }

        size_t rank = 0;
        for (size_t j = 0; j < s_items_storage.count; j++) {
            if (strncmp(items[j].group_id, group_id, sizeof(items[j].group_id)) != 0) {
                continue;
            }
            if (compare_group_items(&items[j], &items[i]) < 0) {
                rank++;
            }
        }

        if (rank == index) {
            *out_item = items[i];
            err = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&s_items_storage.lock);
    return err;
}

size_t gw_group_store_list(gw_group_entry_t *out, size_t max_out)
{
    if (!ready() || !out || max_out == 0) return 0;
    portENTER_CRITICAL(&s_groups_storage.lock);
    const size_t count = s_groups_storage.count < max_out ? s_groups_storage.count : max_out;
    memcpy(out, s_groups_storage.data, count * sizeof(gw_group_entry_t));
    portEXIT_CRITICAL(&s_groups_storage.lock);
    return count;
}

size_t gw_group_store_list_items(gw_group_item_t *out, size_t max_out)
{
    if (!ready() || !out || max_out == 0) return 0;
    portENTER_CRITICAL(&s_items_storage.lock);
    const size_t count = s_items_storage.count < max_out ? s_items_storage.count : max_out;
    memcpy(out, s_items_storage.data, count * sizeof(gw_group_item_t));
    portEXIT_CRITICAL(&s_items_storage.lock);
    return count;
}

esp_err_t gw_group_store_create(const char *id_opt, const char *name, gw_group_entry_t *out_created)
{
    if (!ready() || !name) return ESP_ERR_INVALID_ARG;

    char id[GW_GROUP_ID_MAX] = {0};
    if (id_opt && id_opt[0]) {
        strlcpy(id, id_opt, sizeof(id));
    } else {
        uint32_t seed = now_ms();
        snprintf(id, sizeof(id), "grp_%u", (unsigned)seed);
    }
    if (id[0] == '\0') return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_groups_storage.lock);
    if (find_group_idx(id) != (size_t)-1) {
        portEXIT_CRITICAL(&s_groups_storage.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_groups_storage.count >= GROUPS_MAX) {
        portEXIT_CRITICAL(&s_groups_storage.lock);
        return ESP_ERR_NO_MEM;
    }
    gw_group_entry_t *groups = (gw_group_entry_t *)s_groups_storage.data;
    gw_group_entry_t entry = {0};
    strlcpy(entry.id, id, sizeof(entry.id));
    strlcpy(entry.name, name, sizeof(entry.name));
    entry.version = next_version();
    entry.created_at_ms = now_ms();
    entry.updated_at_ms = entry.created_at_ms;
    groups[s_groups_storage.count++] = entry;
    portEXIT_CRITICAL(&s_groups_storage.lock);

    esp_err_t err = persist_all();
    if (err != ESP_OK) return err;
    if (out_created) *out_created = entry;
    notify_group_listeners();
    return ESP_OK;
}

esp_err_t gw_group_store_rename(const char *id, const char *name)
{
    if (!ready() || !id || !name) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_groups_storage.lock);
    size_t idx = find_group_idx(id);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_groups_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    gw_group_entry_t *groups = (gw_group_entry_t *)s_groups_storage.data;
    strlcpy(groups[idx].name, name, sizeof(groups[idx].name));
    groups[idx].version = next_version();
    groups[idx].updated_at_ms = now_ms();
    portEXIT_CRITICAL(&s_groups_storage.lock);

    esp_err_t err = persist_all();
    if (err == ESP_OK) notify_group_listeners();
    return err;
}

esp_err_t gw_group_store_remove(const char *id)
{
    if (!ready() || !id) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_groups_storage.lock);
    size_t idx = find_group_idx(id);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_groups_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }
    gw_group_entry_t *groups = (gw_group_entry_t *)s_groups_storage.data;
    for (size_t i = idx + 1; i < s_groups_storage.count; i++) {
        groups[i - 1] = groups[i];
    }
    s_groups_storage.count--;
    memset(&groups[s_groups_storage.count], 0, sizeof(gw_group_entry_t));
    portEXIT_CRITICAL(&s_groups_storage.lock);

    portENTER_CRITICAL(&s_items_storage.lock);
    gw_group_item_t *items = (gw_group_item_t *)s_items_storage.data;
    size_t write = 0;
    for (size_t i = 0; i < s_items_storage.count; i++) {
        if (strncmp(items[i].group_id, id, sizeof(items[i].group_id)) == 0) continue;
        if (write != i) items[write] = items[i];
        write++;
    }
    for (size_t i = write; i < s_items_storage.count; i++) {
        memset(&items[i], 0, sizeof(gw_group_item_t));
    }
    s_items_storage.count = write;
    portEXIT_CRITICAL(&s_items_storage.lock);

    esp_err_t err = persist_all();
    if (err == ESP_OK) notify_group_listeners();
    return err;
}

esp_err_t gw_group_store_set_endpoint(const char *group_id, const gw_device_uid_t *device_uid, uint8_t endpoint)
{
    if (!ready() || !group_id || !device_uid || endpoint == 0) return ESP_ERR_INVALID_ARG;
    if (!group_exists(group_id)) return ESP_ERR_NOT_FOUND;

    char moved_from_group_id[GW_GROUP_ID_MAX] = {0};
    const uint32_t updated_at_ms = now_ms();
    portENTER_CRITICAL(&s_items_storage.lock);
    gw_group_item_t *items = (gw_group_item_t *)s_items_storage.data;

    // Unique endpoint membership: endpoint belongs to one custom group at a time.
    size_t existing_idx = (size_t)-1;
    for (size_t i = 0; i < s_items_storage.count; i++) {
        if (uid_equals(&items[i].device_uid, device_uid) && items[i].endpoint == endpoint) {
            if (strncmp(items[i].group_id, group_id, sizeof(items[i].group_id)) == 0) {
                existing_idx = i;
                break;
            }
            // Remove mapping to old group.
            strlcpy(moved_from_group_id, items[i].group_id, sizeof(moved_from_group_id));
            for (size_t j = i + 1; j < s_items_storage.count; j++) {
                items[j - 1] = items[j];
            }
            s_items_storage.count--;
            memset(&items[s_items_storage.count], 0, sizeof(gw_group_item_t));
            break;
        }
    }

    if (existing_idx != (size_t)-1) {
        items[existing_idx].order = updated_at_ms;
        items[existing_idx].version = next_version();
        portEXIT_CRITICAL(&s_items_storage.lock);
        (void)touch_group_version(group_id, updated_at_ms);
        esp_err_t err = persist_all();
        if (err == ESP_OK) notify_group_listeners();
        return err;
    }

    if (s_items_storage.count >= GROUP_ITEMS_MAX) {
        portEXIT_CRITICAL(&s_items_storage.lock);
        return ESP_ERR_NO_MEM;
    }

    gw_group_item_t item = {0};
    strlcpy(item.group_id, group_id, sizeof(item.group_id));
    item.device_uid = *device_uid;
    item.endpoint = endpoint;
    item.version = next_version();
    item.order = updated_at_ms;
    items[s_items_storage.count++] = item;

    portEXIT_CRITICAL(&s_items_storage.lock);
    if (moved_from_group_id[0] != '\0') {
        (void)touch_group_version(moved_from_group_id, updated_at_ms);
    }
    (void)touch_group_version(group_id, updated_at_ms);
    esp_err_t err = persist_all();
    if (err == ESP_OK) notify_group_listeners();
    return err;
}

esp_err_t gw_group_store_remove_endpoint(const gw_device_uid_t *device_uid, uint8_t endpoint)
{
    if (!ready() || !device_uid || endpoint == 0) return ESP_ERR_INVALID_ARG;

    bool removed = false;
    char removed_group_id[GW_GROUP_ID_MAX] = {0};
    const uint32_t updated_at_ms = now_ms();
    portENTER_CRITICAL(&s_items_storage.lock);
    gw_group_item_t *items = (gw_group_item_t *)s_items_storage.data;
    size_t write = 0;
    for (size_t i = 0; i < s_items_storage.count; i++) {
        const bool drop = uid_equals(&items[i].device_uid, device_uid) && items[i].endpoint == endpoint;
        if (drop) {
            removed = true;
            if (removed_group_id[0] == '\0') {
                strlcpy(removed_group_id, items[i].group_id, sizeof(removed_group_id));
            }
            continue;
        }
        if (write != i) items[write] = items[i];
        write++;
    }
    for (size_t i = write; i < s_items_storage.count; i++) {
        memset(&items[i], 0, sizeof(gw_group_item_t));
    }
    s_items_storage.count = write;
    portEXIT_CRITICAL(&s_items_storage.lock);

    if (!removed) return ESP_OK;
    if (removed_group_id[0] != '\0') {
        (void)touch_group_version(removed_group_id, updated_at_ms);
    }
    esp_err_t err = persist_all();
    if (err == ESP_OK) notify_group_listeners();
    return err;
}

esp_err_t gw_group_store_reorder_endpoint(const char *group_id, const gw_device_uid_t *device_uid, uint8_t endpoint, uint32_t order)
{
    if (!ready() || !group_id || !device_uid || endpoint == 0) return ESP_ERR_INVALID_ARG;

    bool updated = false;
    const uint32_t updated_at_ms = now_ms();
    portENTER_CRITICAL(&s_items_storage.lock);
    gw_group_item_t *items = (gw_group_item_t *)s_items_storage.data;
    for (size_t i = 0; i < s_items_storage.count; i++) {
        if (strncmp(items[i].group_id, group_id, sizeof(items[i].group_id)) != 0) continue;
        if (!uid_equals(&items[i].device_uid, device_uid)) continue;
        if (items[i].endpoint != endpoint) continue;
        items[i].order = order;
        items[i].version = next_version();
        updated = true;
        break;
    }
    portEXIT_CRITICAL(&s_items_storage.lock);

    if (!updated) return ESP_ERR_NOT_FOUND;
    (void)touch_group_version(group_id, updated_at_ms);
    esp_err_t err = persist_all();
    if (err == ESP_OK) notify_group_listeners();
    return err;
}

esp_err_t gw_group_store_set_endpoint_label(const gw_device_uid_t *device_uid, uint8_t endpoint, const char *label)
{
    if (!ready() || !device_uid || endpoint == 0 || !label) return ESP_ERR_INVALID_ARG;

    bool updated = false;
    char group_id[GW_GROUP_ID_MAX] = {0};
    const uint32_t updated_at_ms = now_ms();
    portENTER_CRITICAL(&s_items_storage.lock);
    gw_group_item_t *items = (gw_group_item_t *)s_items_storage.data;
    for (size_t i = 0; i < s_items_storage.count; i++) {
        if (!uid_equals(&items[i].device_uid, device_uid)) continue;
        if (items[i].endpoint != endpoint) continue;
        strlcpy(items[i].label, label, sizeof(items[i].label));
        items[i].version = next_version();
        strlcpy(group_id, items[i].group_id, sizeof(group_id));
        updated = true;
        break;
    }
    portEXIT_CRITICAL(&s_items_storage.lock);

    if (!updated) return ESP_ERR_NOT_FOUND;
    if (group_id[0] != '\0') {
        (void)touch_group_version(group_id, updated_at_ms);
    }
    esp_err_t err = persist_all();
    if (err == ESP_OK) notify_group_listeners();
    return err;
}
