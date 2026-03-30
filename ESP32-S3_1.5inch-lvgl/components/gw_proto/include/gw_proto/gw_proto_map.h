#pragma once

#include "gw_core/device_registry.h"
#include "gw_core/project_settings.h"
#include "gw_proto/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

void gw_proto_fill_hdr(gw_proto_hdr_t *out, uint8_t type, uint16_t len, uint16_t seq);
void gw_proto_fill_device(gw_proto_device_v1_t *out, const gw_device_t *src);
void gw_proto_fill_device_remove(gw_proto_device_remove_v1_t *out, const gw_device_uid_t *uid);
void gw_proto_fill_endpoint(gw_proto_endpoint_v1_t *out, const gw_zb_endpoint_t *src);
void gw_proto_fill_endpoint_remove(gw_proto_endpoint_remove_v1_t *out, const gw_device_uid_t *uid, uint8_t endpoint, uint16_t short_addr);
void gw_proto_fill_state_item(gw_proto_state_item_v1_t *out, const gw_state_item_t *src);
void gw_proto_fill_state_remove(gw_proto_state_remove_v1_t *out, const gw_device_uid_t *uid, uint8_t endpoint, const char *key);
void gw_proto_fill_group(gw_proto_group_v1_t *out, const gw_group_entry_t *src);
void gw_proto_fill_group_remove(gw_proto_group_remove_v1_t *out, const char *group_id);
void gw_proto_fill_group_item(gw_proto_group_item_v1_t *out, const gw_group_item_t *src);
void gw_proto_fill_group_item_remove(gw_proto_group_item_remove_v1_t *out, const gw_device_uid_t *uid, uint8_t endpoint);
void gw_proto_fill_settings(gw_proto_settings_v1_t *out, const gw_project_settings_t *src);

#ifdef __cplusplus
}
#endif
