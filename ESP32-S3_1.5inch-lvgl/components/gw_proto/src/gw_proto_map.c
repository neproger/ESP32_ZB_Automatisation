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

void gw_proto_fill_device_remove(gw_proto_device_remove_v1_t *out, const gw_device_uid_t *uid)
{
    if (!out || !uid) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->device_uid = *uid;
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
