#include "gw_proto/gw_proto_validate.h"

#include <string.h>

#include <string.h>

static void trim_fixed_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    memset(dst, 0, dst_size);

    if (!src) {
        return;
    }

    size_t len = strlen(src);
    if (len > dst_size - 1) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
}

void gw_proto_trim_device_uid(gw_device_uid_t *dst, const char *src)
{
    if (!dst) {
        return;
    }
    trim_fixed_string(dst->uid, sizeof(dst->uid), src);
}

void gw_proto_trim_device_v1(gw_proto_device_v1_t *dst, const gw_proto_device_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->device_uid.uid, sizeof(dst->device_uid.uid), src->device_uid.uid);
    dst->short_addr = src->short_addr;
    trim_fixed_string(dst->name, sizeof(dst->name), src->name);
    dst->version = src->version;
    dst->last_seen_ms = src->last_seen_ms;
    dst->has_onoff = src->has_onoff ? 1 : 0;
    dst->has_button = src->has_button ? 1 : 0;
    dst->status = src->status;
}

void gw_proto_trim_endpoint_v1(gw_proto_endpoint_v1_t *dst, const gw_proto_endpoint_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->uid.uid, sizeof(dst->uid.uid), src->uid.uid);
    dst->endpoint = src->endpoint;
    dst->short_addr = src->short_addr;
    dst->version = src->version;
    dst->profile_id = src->profile_id;
    dst->device_id = src->device_id;
    dst->in_cluster_count = src->in_cluster_count;
    dst->out_cluster_count = src->out_cluster_count;

    size_t n = (src->in_cluster_count > GW_ZB_MAX_CLUSTERS) ? GW_ZB_MAX_CLUSTERS : src->in_cluster_count;
    memcpy(dst->in_clusters, src->in_clusters, n * sizeof(dst->in_clusters[0]));

    n = (src->out_cluster_count > GW_ZB_MAX_CLUSTERS) ? GW_ZB_MAX_CLUSTERS : src->out_cluster_count;
    memcpy(dst->out_clusters, src->out_clusters, n * sizeof(dst->out_clusters[0]));

    trim_fixed_string(dst->kind, sizeof(dst->kind), src->kind);
}

void gw_proto_trim_state_item_v1(gw_proto_state_item_v1_t *dst, const gw_proto_state_item_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->uid.uid, sizeof(dst->uid.uid), src->uid.uid);
    dst->endpoint = src->endpoint;
    dst->value_type = src->value_type;
    trim_fixed_string(dst->key, sizeof(dst->key), src->key);
    dst->version = src->version;

    switch (src->value_type) {
        case GW_STATE_VALUE_BOOL:
            dst->value_bool = src->value_bool ? 1 : 0;
            break;
        case GW_STATE_VALUE_F32:
            dst->value_f32 = src->value_f32;
            break;
        case GW_STATE_VALUE_U32:
            dst->value_u32 = src->value_u32;
            break;
        case GW_STATE_VALUE_U64:
            dst->value_u64 = src->value_u64;
            break;
        case GW_STATE_VALUE_TEXT:
            trim_fixed_string(dst->value_text, sizeof(dst->value_text), src->value_text);
            break;
        default:
            break;
    }

    dst->ts_ms = src->ts_ms;
}

void gw_proto_trim_state_remove_v1(gw_proto_state_remove_v1_t *dst, const gw_proto_state_remove_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->uid.uid, sizeof(dst->uid.uid), src->uid.uid);
    dst->endpoint = src->endpoint;
    trim_fixed_string(dst->key, sizeof(dst->key), src->key);
}

void gw_proto_trim_group_v1(gw_proto_group_v1_t *dst, const gw_proto_group_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->id, sizeof(dst->id), src->id);
    trim_fixed_string(dst->name, sizeof(dst->name), src->name);
    dst->version = src->version;
    dst->created_at_ms = src->created_at_ms;
    dst->updated_at_ms = src->updated_at_ms;
}

void gw_proto_trim_group_remove_v1(gw_proto_group_remove_v1_t *dst, const gw_proto_group_remove_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->id, sizeof(dst->id), src->id);
}

void gw_proto_trim_group_item_v1(gw_proto_group_item_v1_t *dst, const gw_proto_group_item_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->group_id, sizeof(dst->group_id), src->group_id);
    trim_fixed_string(dst->device_uid.uid, sizeof(dst->device_uid.uid), src->device_uid.uid);
    dst->endpoint = src->endpoint;
    dst->version = src->version;
    dst->order = src->order;
    trim_fixed_string(dst->label, sizeof(dst->label), src->label);
}

void gw_proto_trim_automation_entry(gw_automation_entry_t *dst, const gw_automation_entry_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->id, sizeof(dst->id), src->id);
    trim_fixed_string(dst->name, sizeof(dst->name), src->name);
    dst->enabled = src->enabled ? 1 : 0;
    dst->triggers_count = src->triggers_count;
    dst->conditions_count = src->conditions_count;
    dst->actions_count = src->actions_count;

    size_t n = (src->triggers_count > GW_AUTO_MAX_TRIGGERS) ? GW_AUTO_MAX_TRIGGERS : src->triggers_count;
    memcpy(dst->triggers, src->triggers, n * sizeof(dst->triggers[0]));

    n = (src->conditions_count > GW_AUTO_MAX_CONDITIONS) ? GW_AUTO_MAX_CONDITIONS : src->conditions_count;
    memcpy(dst->conditions, src->conditions, n * sizeof(dst->conditions[0]));

    n = (src->actions_count > GW_AUTO_MAX_ACTIONS) ? GW_AUTO_MAX_ACTIONS : src->actions_count;
    memcpy(dst->actions, src->actions, n * sizeof(dst->actions[0]));

    dst->string_table_size = src->string_table_size;
    n = (src->string_table_size > GW_AUTO_MAX_STRING_TABLE_BYTES) ? GW_AUTO_MAX_STRING_TABLE_BYTES : src->string_table_size;
    memcpy(dst->string_table, src->string_table, n);
}

void gw_proto_trim_event_v1(gw_proto_event_v1_t *dst, const gw_proto_event_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    dst->event_id = src->event_id;
    dst->ts_ms = src->ts_ms;
    dst->event_id_kind = src->event_id_kind;
    trim_fixed_string(dst->cmd, sizeof(dst->cmd), src->cmd);
    trim_fixed_string(dst->device_uid.uid, sizeof(dst->device_uid.uid), src->device_uid.uid);
    dst->short_addr = src->short_addr;
    dst->endpoint = src->endpoint;
    dst->cluster_id = src->cluster_id;
    dst->attr_id = src->attr_id;
    dst->value_type = src->value_type;

    switch (src->value_type) {
        case GW_PROTO_EVENT_VALUE_BOOL:
            dst->value_bool = src->value_bool ? 1 : 0;
            break;
        case GW_PROTO_EVENT_VALUE_I64:
            dst->value_i64 = src->value_i64;
            break;
        case GW_PROTO_EVENT_VALUE_F32:
            dst->value_f32 = src->value_f32;
            break;
        case GW_PROTO_EVENT_VALUE_TEXT:
            trim_fixed_string(dst->value_text, sizeof(dst->value_text), src->value_text);
            break;
        default:
            break;
    }

    dst->status_code = src->status_code;
    dst->aux_u16 = src->aux_u16;
    dst->parent_short_addr = src->parent_short_addr;
    dst->flags = src->flags;
}

void gw_proto_trim_trace_v1(gw_proto_trace_v1_t *dst, const gw_proto_trace_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    dst->v = src->v;
    dst->kind = src->kind;
    dst->ok = src->ok ? 1 : 0;
    dst->id = src->id;
    dst->ts_ms = src->ts_ms;
    trim_fixed_string(dst->device_uid, sizeof(dst->device_uid), src->device_uid);
    dst->short_addr = src->short_addr;
    dst->action_index = src->action_index;
    trim_fixed_string(dst->automation_id, sizeof(dst->automation_id), src->automation_id);
    trim_fixed_string(dst->error_text, sizeof(dst->error_text), src->error_text);
}

void gw_proto_trim_cmd_group_create_v1(gw_proto_cmd_group_create_v1_t *dst, const gw_proto_cmd_group_create_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->id, sizeof(dst->id), src->id);
    trim_fixed_string(dst->name, sizeof(dst->name), src->name);
}

void gw_proto_trim_cmd_group_delete_v1(gw_proto_cmd_group_delete_v1_t *dst, const gw_proto_cmd_group_delete_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->id, sizeof(dst->id), src->id);
}

void gw_proto_trim_cmd_automation_remove_v1(gw_proto_cmd_automation_remove_v1_t *dst, const gw_proto_cmd_automation_remove_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->id, sizeof(dst->id), src->id);
}

void gw_proto_trim_device_remove_v1(gw_proto_device_remove_v1_t *dst, const gw_proto_device_remove_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->device_uid.uid, sizeof(dst->device_uid.uid), src->device_uid.uid);
}

void gw_proto_trim_automation_remove_v1(gw_proto_automation_remove_v1_t *dst, const gw_proto_automation_remove_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->id, sizeof(dst->id), src->id);
}

void gw_proto_trim_settings_v1(gw_proto_settings_v1_t *dst, const gw_proto_settings_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    dst->screensaver_timeout_ms = src->screensaver_timeout_ms;
    dst->weather_success_interval_ms = src->weather_success_interval_ms;
    dst->weather_retry_interval_ms = src->weather_retry_interval_ms;
    dst->timezone_auto = src->timezone_auto ? 1 : 0;
    dst->timezone_offset_min = src->timezone_offset_min;
}

void gw_proto_trim_endpoint_remove_v1(gw_proto_endpoint_remove_v1_t *dst, const gw_proto_endpoint_remove_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->uid.uid, sizeof(dst->uid.uid), src->uid.uid);
    dst->endpoint = src->endpoint;
    dst->short_addr = src->short_addr;
}

void gw_proto_trim_group_item_remove_v1(gw_proto_group_item_remove_v1_t *dst, const gw_proto_group_item_remove_v1_t *src)
{
    if (!dst || !src) {
        return;
    }

    trim_fixed_string(dst->device_uid.uid, sizeof(dst->device_uid.uid), src->device_uid.uid);
    dst->endpoint = src->endpoint;
}
