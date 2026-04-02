#include "gw_core/automation_store.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gw_core/gw_proto_bus.h"
#include "gw_model/gw_model_automation.h"
#include "gw_proto/gw_proto_map.h"

static const char *TAG = "gw_autos";

static bool s_initialized = false;

#define GW_AUTOMATION_LISTENER_CAP 4
typedef struct {
    gw_automation_store_listener_t cb;
    void *user_ctx;
} gw_automation_listener_slot_t;

static gw_automation_listener_slot_t s_listeners[GW_AUTOMATION_LISTENER_CAP];
static portMUX_TYPE s_listener_lock = portMUX_INITIALIZER_UNLOCKED;

static void notify_automation_listeners(void)
{
    gw_automation_listener_slot_t listeners[GW_AUTOMATION_LISTENER_CAP];
    size_t listener_count = 0;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_AUTOMATION_LISTENER_CAP; i++) {
        if (s_listeners[i].cb) {
            listeners[listener_count++] = s_listeners[i];
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < listener_count; i++) {
        listeners[i].cb(listeners[i].user_ctx);
    }
}

static void publish_automation_upsert(const gw_automation_entry_t *entry)
{
    if (!entry || entry->id[0] == '\0') {
        return;
    }
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_AUTOMATION_UPSERT, sizeof(*entry), 0);
    (void)gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, entry);
}

static void publish_automation_remove(const char *id)
{
    if (!id || id[0] == '\0') {
        return;
    }
    gw_proto_automation_remove_v1_t msg = {0};
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_automation_remove(&msg, id);
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_AUTOMATION_REMOVE, sizeof(msg), 0);
    (void)gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
}

static esp_err_t validate_automation_entry(const gw_automation_entry_t *entry)
{
    if (!entry) {
        return ESP_ERR_INVALID_ARG;
    }
    if (entry->id[0] == '\0' || entry->name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(entry->id) >= GW_AUTOMATION_ID_MAX || strlen(entry->name) >= GW_AUTOMATION_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (entry->triggers_count > GW_AUTO_MAX_TRIGGERS ||
        entry->conditions_count > GW_AUTO_MAX_CONDITIONS ||
        entry->actions_count > GW_AUTO_MAX_ACTIONS ||
        entry->string_table_size > GW_AUTO_MAX_STRING_TABLE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t gw_automation_store_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Automation storage initialized with %zu automations", gw_model_count_automations());
    return ESP_OK;
}

esp_err_t gw_automation_store_add_listener(gw_automation_store_listener_t cb, void *user_ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_AUTOMATION_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < GW_AUTOMATION_LISTENER_CAP; i++) {
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

esp_err_t gw_automation_store_remove_listener(gw_automation_store_listener_t cb, void *user_ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_AUTOMATION_LISTENER_CAP; i++) {
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

size_t gw_automation_store_count(void)
{
    return s_initialized ? gw_model_count_automations() : 0;
}

size_t gw_automation_store_list(gw_automation_entry_t *out, size_t max_out)
{
    if (!s_initialized) {
        return 0;
    }
    return gw_model_list_automations(out, max_out);
}

size_t gw_automation_store_list_meta(gw_automation_meta_t *out, size_t max_out)
{
    if (!s_initialized || !out || max_out == 0) {
        return 0;
    }

    gw_automation_entry_t tmp[32] = {0};
    const size_t count = gw_model_list_automations(tmp, max_out < 32 ? max_out : 32);
    for (size_t i = 0; i < count; i++) {
        strlcpy(out[i].id, tmp[i].id, sizeof(out[i].id));
        strlcpy(out[i].name, tmp[i].name, sizeof(out[i].name));
        out[i].enabled = tmp[i].enabled;
    }
    return count;
}

esp_err_t gw_automation_store_get(const char *id, gw_automation_entry_t *out)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return gw_model_get_automation(id, out);
}

esp_err_t gw_automation_store_get_by_index(size_t index, gw_automation_entry_t *out)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return gw_model_get_automation_by_index(index, out);
}

esp_err_t gw_automation_store_put_entry(const gw_automation_entry_t *entry)
{
    if (!s_initialized || !entry) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = validate_automation_entry(entry);
    if (err != ESP_OK) {
        return err;
    }

    err = gw_model_upsert_automation(entry, NULL, NULL);
    if (err == ESP_OK) {
        publish_automation_upsert(entry);
        notify_automation_listeners();
    }
    return err;
}

esp_err_t gw_automation_store_remove(const char *id)
{
    if (!s_initialized || !id) {
        return ESP_ERR_INVALID_ARG;
    }

    bool removed = false;
    esp_err_t err = gw_model_remove_automation(id, &removed);
    if (err == ESP_OK && removed) {
        publish_automation_remove(id);
        notify_automation_listeners();
    }
    return err;
}

esp_err_t gw_automation_store_set_enabled(const char *id, bool enabled)
{
    if (!s_initialized || !id) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_automation_entry_t entry = {0};
    esp_err_t err = gw_model_get_automation(id, &entry);
    if (err != ESP_OK) {
        return err;
    }
    entry.enabled = enabled ? 1u : 0u;
    err = gw_model_upsert_automation(&entry, NULL, NULL);
    if (err == ESP_OK) {
        publish_automation_upsert(&entry);
        notify_automation_listeners();
    }
    return err;
}

esp_err_t gw_automation_store_remove_all(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_automation_entry_t removed[32] = {0};
    const size_t count = gw_model_list_automations(removed, 32);
    for (size_t i = 0; i < count; i++) {
        bool deleted = false;
        esp_err_t err = gw_model_remove_automation(removed[i].id, &deleted);
        if (err != ESP_OK) {
            return err;
        }
    }
    for (size_t i = 0; i < count; i++) {
        publish_automation_remove(removed[i].id);
    }
    notify_automation_listeners();
    return ESP_OK;
}
