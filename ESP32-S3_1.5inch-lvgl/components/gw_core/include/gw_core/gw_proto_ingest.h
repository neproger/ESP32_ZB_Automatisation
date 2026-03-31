#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gw_core/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

void gw_proto_ingest_publish_event(const gw_proto_event_v1_t *evt);

esp_err_t gw_proto_ingest_apply_sync_begin(const gw_proto_sync_begin_v1_t *msg);
esp_err_t gw_proto_ingest_apply_sync_end(const gw_proto_sync_end_v1_t *msg, bool publish_sync_ready);
esp_err_t gw_proto_ingest_apply_device(const gw_proto_device_v1_t *msg);
esp_err_t gw_proto_ingest_apply_endpoint(const gw_proto_endpoint_v1_t *msg);
esp_err_t gw_proto_ingest_apply_state_item(const gw_proto_state_item_v1_t *msg);
esp_err_t gw_proto_ingest_apply_device_remove(const gw_proto_device_remove_v1_t *msg);

#ifdef __cplusplus
}
#endif
