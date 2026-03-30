#include "gw_proto/gw_proto_map.h"

#include <string.h>

void gw_proto_fill_hdr(gw_proto_hdr_t *out, uint8_t type, uint16_t len, uint16_t seq)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->version = GW_PROTO_VERSION_V1;
    out->type = type;
    out->len = len;
    out->seq = seq;
}

void gw_proto_fill_device(gw_proto_device_v1_t *out, const gw_device_t *src)
{
    if (!out || !src) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->device_uid = src->device_uid;
    out->short_addr = src->short_addr;
    memcpy(out->name, src->name, sizeof(out->name));
    out->version = src->version;
    out->last_seen_ms = src->last_seen_ms;
    out->has_onoff = src->has_onoff ? 1u : 0u;
    out->has_button = src->has_button ? 1u : 0u;
}

void gw_proto_fill_device_remove(gw_proto_device_remove_v1_t *out, const gw_device_uid_t *uid)
{
    if (!out || !uid) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->device_uid = *uid;
}

void gw_proto_fill_endpoint(gw_proto_endpoint_v1_t *out, const gw_zb_endpoint_t *src)
{
    if (!out || !src) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->uid = src->uid;
    out->short_addr = src->short_addr;
    out->endpoint = src->endpoint;
    out->version = src->version;
    out->profile_id = src->profile_id;
    out->device_id = src->device_id;
    out->in_cluster_count = src->in_cluster_count;
    out->out_cluster_count = src->out_cluster_count;
    memcpy(out->in_clusters, src->in_clusters, sizeof(out->in_clusters));
    memcpy(out->out_clusters, src->out_clusters, sizeof(out->out_clusters));
}

void gw_proto_fill_endpoint_remove(gw_proto_endpoint_remove_v1_t *out, const gw_device_uid_t *uid, uint8_t endpoint, uint16_t short_addr)
{
    if (!out || !uid) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->uid = *uid;
    out->endpoint = endpoint;
    out->short_addr = short_addr;
}

void gw_proto_fill_state_item(gw_proto_state_item_v1_t *out, const gw_state_item_t *src)
{
    if (!out || !src) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->uid = src->uid;
    out->endpoint = src->endpoint;
    out->value_type = (uint8_t)src->value_type;
    memcpy(out->key, src->key, sizeof(out->key));
    out->version = src->version;
    out->value_bool = src->value_bool ? 1u : 0u;
    out->value_f32 = src->value_f32;
    out->value_u32 = src->value_u32;
    out->value_u64 = src->value_u64;
    memcpy(out->value_text, src->value_text, sizeof(out->value_text));
    out->ts_ms = src->ts_ms;
}

void gw_proto_fill_state_remove(gw_proto_state_remove_v1_t *out, const gw_device_uid_t *uid, uint8_t endpoint, const char *key)
{
    if (!out || !uid) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->uid = *uid;
    out->endpoint = endpoint;
    if (key) {
        strlcpy(out->key, key, sizeof(out->key));
    }
}

void gw_proto_fill_group(gw_proto_group_v1_t *out, const gw_group_entry_t *src)
{
    if (!out || !src) {
        return;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->id, src->id, sizeof(out->id));
    memcpy(out->name, src->name, sizeof(out->name));
    out->version = src->version;
    out->created_at_ms = src->created_at_ms;
    out->updated_at_ms = src->updated_at_ms;
}

void gw_proto_fill_group_remove(gw_proto_group_remove_v1_t *out, const char *group_id)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (group_id) {
        strlcpy(out->id, group_id, sizeof(out->id));
    }
}

void gw_proto_fill_group_item(gw_proto_group_item_v1_t *out, const gw_group_item_t *src)
{
    if (!out || !src) {
        return;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->group_id, src->group_id, sizeof(out->group_id));
    out->device_uid = src->device_uid;
    out->endpoint = src->endpoint;
    out->version = src->version;
    out->order = src->order;
    memcpy(out->label, src->label, sizeof(out->label));
}

void gw_proto_fill_group_item_remove(gw_proto_group_item_remove_v1_t *out, const gw_device_uid_t *uid, uint8_t endpoint)
{
    if (!out || !uid) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->device_uid = *uid;
    out->endpoint = endpoint;
}

void gw_proto_fill_settings(gw_proto_settings_v1_t *out, const gw_project_settings_t *src)
{
    if (!out || !src) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->screensaver_timeout_ms = src->screensaver_timeout_ms;
    out->weather_success_interval_ms = src->weather_success_interval_ms;
    out->weather_retry_interval_ms = src->weather_retry_interval_ms;
    out->timezone_auto = src->timezone_auto ? 1u : 0u;
    out->timezone_offset_min = src->timezone_offset_min;
}

void gw_proto_fill_automation_remove(gw_proto_automation_remove_v1_t *out, const char *automation_id)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (automation_id) {
        strlcpy(out->id, automation_id, sizeof(out->id));
    }
}
