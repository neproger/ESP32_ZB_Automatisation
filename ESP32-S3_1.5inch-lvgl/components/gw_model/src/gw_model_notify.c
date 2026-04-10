#include "gw_model_notify.h"

#include <string.h>

#include "esp_log.h"
#include "gw_core/gw_proto_bus.h"
#include "gw_proto/gw_proto_map.h"

esp_err_t gw_model_notify_device_upsert(const gw_proto_device_v1_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_DEVICE_UPSERT, sizeof(*record), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, record);
}

esp_err_t gw_model_notify_device_remove(const gw_device_uid_t *uid)
{
    if (!uid) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_device_remove_v1_t msg = {0};
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_device_remove(&msg, uid);
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_DEVICE_REMOVE, sizeof(msg), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
}

esp_err_t gw_model_notify_endpoint_upsert(const gw_proto_endpoint_v1_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_ENDPOINT_UPSERT, sizeof(*record), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, record);
}

esp_err_t gw_model_notify_endpoint_remove(const gw_model_endpoint_key_t *key, uint16_t short_addr)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_endpoint_remove_v1_t msg = {0};
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_endpoint_remove(&msg, &key->uid, key->endpoint, short_addr);
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_ENDPOINT_REMOVE, sizeof(msg), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
}

esp_err_t gw_model_notify_state_upsert(const gw_proto_state_item_v1_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_STATE_ITEM, sizeof(*record), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, record);
}

esp_err_t gw_model_notify_state_remove(const gw_model_state_key_t *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_state_remove_v1_t msg = {0};
    gw_proto_hdr_t hdr = {0};
    msg.uid = key->uid;
    msg.endpoint = key->endpoint;
    memcpy(msg.key, key->key, sizeof(msg.key));
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_STATE_REMOVE, sizeof(msg), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
}

esp_err_t gw_model_notify_group_upsert(const gw_proto_group_v1_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_GROUP_UPSERT, sizeof(*record), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, record);
}

esp_err_t gw_model_notify_group_remove(const char *group_id)
{
    if (!group_id) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_group_remove_v1_t msg = {0};
    gw_proto_hdr_t hdr = {0};
    strlcpy(msg.id, group_id, sizeof(msg.id));
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_GROUP_REMOVE, sizeof(msg), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
}

esp_err_t gw_model_notify_group_item_upsert(const gw_proto_group_item_v1_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_GROUP_ITEM_UPSERT, sizeof(*record), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, record);
}

esp_err_t gw_model_notify_group_item_remove(const gw_model_endpoint_key_t *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_group_item_remove_v1_t msg = {0};
    gw_proto_hdr_t hdr = {0};
    msg.device_uid = key->uid;
    msg.endpoint = key->endpoint;
    gw_proto_fill_group_item_remove(&msg, &key->uid, key->endpoint);
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_GROUP_ITEM_REMOVE, sizeof(msg), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
}

esp_err_t gw_model_notify_settings(const gw_proto_settings_v1_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_SETTINGS, sizeof(*record), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, record);
}

esp_err_t gw_model_notify_automation_upsert(const gw_automation_entry_t *record)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_hdr_t hdr = {0};
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_AUTOMATION_UPSERT, sizeof(*record), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, record);
}

esp_err_t gw_model_notify_automation_remove(const char *id)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_proto_cmd_automation_remove_v1_t msg = {0};
    gw_proto_hdr_t hdr = {0};
    strlcpy(msg.id, id, sizeof(msg.id));
    gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_AUTOMATION_REMOVE, sizeof(msg), 0);
    return gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
}
