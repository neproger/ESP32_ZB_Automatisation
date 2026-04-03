#include "gw_core/deleted_devices.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "gw_core/device_registry.h"
#include "gw_core/storage.h"

static const char *TAG = "gw_deleted_devices";

#define GW_DELETED_DEVICE_MAX_ITEMS GW_DEVICE_MAX_DEVICES

static const uint32_t DELETED_DEVICE_STORAGE_MAGIC = 0x44454C44;
static const uint16_t DELETED_DEVICE_STORAGE_VERSION = 1;

static gw_storage_t s_deleted_storage;
static bool s_initialized;

static const gw_storage_desc_t s_deleted_storage_desc = {
    .key = "deleted_devices",
    .item_size = sizeof(gw_deleted_device_t),
    .max_items = GW_DELETED_DEVICE_MAX_ITEMS,
    .magic = DELETED_DEVICE_STORAGE_MAGIC,
    .version = DELETED_DEVICE_STORAGE_VERSION,
    .namespace = "gw",
};

static bool uid_equals(const gw_device_uid_t *a, const gw_device_uid_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a->uid, b->uid, sizeof(a->uid)) == 0;
}

static size_t find_index_by_uid(const gw_device_uid_t *uid)
{
    if (!s_initialized || uid == NULL || uid->uid[0] == '\0') {
        return (size_t)-1;
    }

    const gw_deleted_device_t *items = (const gw_deleted_device_t *)s_deleted_storage.data;
    for (size_t i = 0; i < s_deleted_storage.count; ++i) {
        if (uid_equals(uid, &items[i].device_uid)) {
            return i;
        }
    }
    return (size_t)-1;
}

esp_err_t gw_deleted_devices_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = gw_storage_init(&s_deleted_storage, &s_deleted_storage_desc, GW_STORAGE_NVS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize deleted-device storage: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Deleted-device quarantine initialized with %zu entries", s_deleted_storage.count);
    return ESP_OK;
}

esp_err_t gw_deleted_devices_add(const gw_device_uid_t *uid, uint64_t removed_at_ms)
{
    if (!s_initialized || uid == NULL || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_deleted_storage.lock);

    size_t idx = find_index_by_uid(uid);
    gw_deleted_device_t *items = (gw_deleted_device_t *)s_deleted_storage.data;

    if (idx != (size_t)-1) {
        items[idx].removed_at_ms = removed_at_ms;
        portEXIT_CRITICAL(&s_deleted_storage.lock);
        return gw_storage_save(&s_deleted_storage);
    }

    if (s_deleted_storage.count >= s_deleted_storage.desc->max_items) {
        portEXIT_CRITICAL(&s_deleted_storage.lock);
        return ESP_ERR_NO_MEM;
    }

    gw_deleted_device_t *slot = &items[s_deleted_storage.count++];
    memset(slot, 0, sizeof(*slot));
    slot->device_uid = *uid;
    slot->removed_at_ms = removed_at_ms;

    portEXIT_CRITICAL(&s_deleted_storage.lock);
    return gw_storage_save(&s_deleted_storage);
}

esp_err_t gw_deleted_devices_remove(const gw_device_uid_t *uid)
{
    if (!s_initialized || uid == NULL || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_deleted_storage.lock);

    size_t idx = find_index_by_uid(uid);
    if (idx == (size_t)-1) {
        portEXIT_CRITICAL(&s_deleted_storage.lock);
        return ESP_ERR_NOT_FOUND;
    }

    gw_deleted_device_t *items = (gw_deleted_device_t *)s_deleted_storage.data;
    for (size_t i = idx + 1; i < s_deleted_storage.count; ++i) {
        items[i - 1] = items[i];
    }
    s_deleted_storage.count--;
    memset(&items[s_deleted_storage.count], 0, sizeof(gw_deleted_device_t));

    portEXIT_CRITICAL(&s_deleted_storage.lock);
    return gw_storage_save(&s_deleted_storage);
}

bool gw_deleted_devices_contains(const gw_device_uid_t *uid)
{
    if (!s_initialized || uid == NULL || uid->uid[0] == '\0') {
        return false;
    }

    portENTER_CRITICAL(&s_deleted_storage.lock);
    const bool found = (find_index_by_uid(uid) != (size_t)-1);
    portEXIT_CRITICAL(&s_deleted_storage.lock);
    return found;
}

size_t gw_deleted_devices_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    return s_deleted_storage.count;
}
