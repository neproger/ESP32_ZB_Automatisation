#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_proto/gw_proto_types.h"
#include "gw_proto/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

void gw_proto_trim_device_uid(gw_device_uid_t *dst, const char *src);

void gw_proto_trim_device_v1(gw_proto_device_v1_t *dst, const gw_proto_device_v1_t *src);

void gw_proto_trim_endpoint_v1(gw_proto_endpoint_v1_t *dst, const gw_proto_endpoint_v1_t *src);

void gw_proto_trim_state_item_v1(gw_proto_state_item_v1_t *dst, const gw_proto_state_item_v1_t *src);

void gw_proto_trim_state_remove_v1(gw_proto_state_remove_v1_t *dst, const gw_proto_state_remove_v1_t *src);

void gw_proto_trim_group_v1(gw_proto_group_v1_t *dst, const gw_proto_group_v1_t *src);

void gw_proto_trim_group_remove_v1(gw_proto_group_remove_v1_t *dst, const gw_proto_group_remove_v1_t *src);

void gw_proto_trim_group_item_v1(gw_proto_group_item_v1_t *dst, const gw_proto_group_item_v1_t *src);

void gw_proto_trim_automation_entry(gw_automation_entry_t *dst, const gw_automation_entry_t *src);

void gw_proto_trim_event_v1(gw_proto_event_v1_t *dst, const gw_proto_event_v1_t *src);

void gw_proto_trim_trace_v1(gw_proto_trace_v1_t *dst, const gw_proto_trace_v1_t *src);

void gw_proto_trim_cmd_group_create_v1(gw_proto_cmd_group_create_v1_t *dst, const gw_proto_cmd_group_create_v1_t *src);

void gw_proto_trim_cmd_group_delete_v1(gw_proto_cmd_group_delete_v1_t *dst, const gw_proto_cmd_group_delete_v1_t *src);

void gw_proto_trim_cmd_automation_remove_v1(gw_proto_cmd_automation_remove_v1_t *dst, const gw_proto_cmd_automation_remove_v1_t *src);

void gw_proto_trim_device_remove_v1(gw_proto_device_remove_v1_t *dst, const gw_proto_device_remove_v1_t *src);

void gw_proto_trim_automation_remove_v1(gw_proto_automation_remove_v1_t *dst, const gw_proto_automation_remove_v1_t *src);

void gw_proto_trim_settings_v1(gw_proto_settings_v1_t *dst, const gw_proto_settings_v1_t *src);

void gw_proto_trim_endpoint_remove_v1(gw_proto_endpoint_remove_v1_t *dst, const gw_proto_endpoint_remove_v1_t *src);

void gw_proto_trim_group_item_remove_v1(gw_proto_group_item_remove_v1_t *dst, const gw_proto_group_item_remove_v1_t *src);

#ifdef __cplusplus
}
#endif
