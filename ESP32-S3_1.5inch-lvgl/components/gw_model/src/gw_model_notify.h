#pragma once

#include <stdbool.h>

#include "gw_proto/gw_proto.h"
#include "gw_model/gw_model_schema.h"
#include "esp_err.h"

esp_err_t gw_model_notify_device_upsert(const gw_proto_device_v1_t *record);
esp_err_t gw_model_notify_device_remove(const gw_device_uid_t *uid);

esp_err_t gw_model_notify_endpoint_upsert(const gw_proto_endpoint_v1_t *record);
esp_err_t gw_model_notify_endpoint_remove(const gw_model_endpoint_key_t *key, uint16_t short_addr);

esp_err_t gw_model_notify_state_upsert(const gw_proto_state_item_v1_t *record);
esp_err_t gw_model_notify_state_remove(const gw_model_state_key_t *key);

esp_err_t gw_model_notify_group_upsert(const gw_proto_group_v1_t *record);
esp_err_t gw_model_notify_group_remove(const char *group_id);

esp_err_t gw_model_notify_group_item_upsert(const gw_proto_group_item_v1_t *record);
esp_err_t gw_model_notify_group_item_remove(const gw_model_endpoint_key_t *key);

esp_err_t gw_model_notify_settings(const gw_proto_settings_v1_t *record);
esp_err_t gw_model_notify_automation_upsert(const gw_automation_entry_t *record);
esp_err_t gw_model_notify_automation_remove(const char *id);
