#include "gw_core/gw_proto_ingest.h"

#include <string.h>

#include "gw_core/gw_proto_bus.h"
#include "gw_core/runtime_sync.h"
#include "gw_core/state_store.h"

static bool s_inited;

static void gw_proto_ingest_listener(gw_proto_bus_channel_t channel, const gw_proto_hdr_t *hdr, const void *payload, void *user_ctx)
{
    (void)channel;
    (void)user_ctx;
    if (!hdr) {
        return;
    }

    switch ((gw_proto_msg_type_t)hdr->type) {
        case GW_PROTO_MSG_EVENT_ZB: {
            return;
        }
        case GW_PROTO_MSG_SYNC_BEGIN: {
            if (!payload || hdr->len < sizeof(gw_proto_sync_begin_v1_t)) {
                return;
            }
            (void)gw_proto_ingest_apply_sync_begin((const gw_proto_sync_begin_v1_t *)payload);
            return;
        }
        case GW_PROTO_MSG_SYNC_END: {
            if (!payload || hdr->len < sizeof(gw_proto_sync_end_v1_t)) {
                return;
            }
            (void)gw_proto_ingest_apply_sync_end((const gw_proto_sync_end_v1_t *)payload, true);
            return;
        }
        case GW_PROTO_MSG_DEVICE_UPSERT: {
            if (!payload || hdr->len < sizeof(gw_proto_device_v1_t)) {
                return;
            }
            (void)gw_proto_ingest_apply_device((const gw_proto_device_v1_t *)payload);
            return;
        }
        case GW_PROTO_MSG_DEVICE_REMOVE: {
            if (!payload || hdr->len < sizeof(gw_proto_device_remove_v1_t)) {
                return;
            }
            (void)gw_proto_ingest_apply_device_remove((const gw_proto_device_remove_v1_t *)payload);
            return;
        }
        case GW_PROTO_MSG_ENDPOINT_UPSERT: {
            if (!payload || hdr->len < sizeof(gw_proto_endpoint_v1_t)) {
                return;
            }
            (void)gw_proto_ingest_apply_endpoint((const gw_proto_endpoint_v1_t *)payload);
            return;
        }
        case GW_PROTO_MSG_STATE_ITEM: {
            if (!payload || hdr->len < sizeof(gw_proto_state_item_v1_t)) {
                return;
            }
            (void)gw_proto_ingest_apply_state_item((const gw_proto_state_item_v1_t *)payload);
            return;
        }
        default:
            return;
    }
}

esp_err_t gw_proto_ingest_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = gw_proto_bus_add_listener(gw_proto_ingest_listener, GW_PROTO_BUS_CHANNEL_INGRESS, NULL);
    if (err == ESP_OK) {
        s_inited = true;
    }
    return err;
}

esp_err_t gw_proto_ingest_apply_sync_begin(const gw_proto_sync_begin_v1_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    return gw_runtime_sync_snapshot_begin((uint16_t)msg->total_records);
}

esp_err_t gw_proto_ingest_apply_sync_end(const gw_proto_sync_end_v1_t *msg, bool publish_sync_ready)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = gw_runtime_sync_snapshot_end();
    (void)publish_sync_ready;
    return err;
}

esp_err_t gw_proto_ingest_apply_device(const gw_proto_device_v1_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_device_t d = {0};
    d.device_uid = msg->device_uid;
    d.short_addr = msg->short_addr;
    memcpy(d.name, msg->name, sizeof(d.name));
    d.version = msg->version;
    d.last_seen_ms = msg->last_seen_ms;
    d.has_onoff = (msg->has_onoff != 0);
    d.has_button = (msg->has_button != 0);
    return gw_runtime_sync_snapshot_upsert_device(&d);
}

esp_err_t gw_proto_ingest_apply_endpoint(const gw_proto_endpoint_v1_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_zb_endpoint_t ep = {0};
    ep.uid = msg->uid;
    ep.short_addr = msg->short_addr;
    ep.endpoint = msg->endpoint;
    ep.version = msg->version;
    ep.profile_id = msg->profile_id;
    ep.device_id = msg->device_id;
    ep.in_cluster_count = msg->in_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : msg->in_cluster_count;
    ep.out_cluster_count = msg->out_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : msg->out_cluster_count;
    if (ep.in_cluster_count > 0) {
        memcpy(ep.in_clusters, msg->in_clusters, ep.in_cluster_count * sizeof(uint16_t));
    }
    if (ep.out_cluster_count > 0) {
        memcpy(ep.out_clusters, msg->out_clusters, ep.out_cluster_count * sizeof(uint16_t));
    }
    return gw_runtime_sync_snapshot_upsert_endpoint(&ep);
}

esp_err_t gw_proto_ingest_apply_state_item(const gw_proto_state_item_v1_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    const gw_device_uid_t *uid = &msg->uid;
    const uint8_t endpoint = msg->endpoint;
    const char *key = msg->key;
    switch ((gw_state_value_type_t)msg->value_type) {
        case GW_STATE_VALUE_BOOL:
            return gw_state_store_set_bool(uid, endpoint, key, msg->value_bool != 0, msg->ts_ms);
        case GW_STATE_VALUE_F32:
            return gw_state_store_set_f32(uid, endpoint, key, msg->value_f32, msg->ts_ms);
        case GW_STATE_VALUE_U32:
            return gw_state_store_set_u32(uid, endpoint, key, msg->value_u32, msg->ts_ms);
        case GW_STATE_VALUE_U64:
            return gw_state_store_set_u64(uid, endpoint, key, msg->value_u64, msg->ts_ms);
        case GW_STATE_VALUE_TEXT:
            return gw_state_store_set_text(uid, endpoint, key, msg->value_text, msg->ts_ms);
        default:
            return ESP_OK;
    }
}

esp_err_t gw_proto_ingest_apply_device_remove(const gw_proto_device_remove_v1_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    return gw_runtime_sync_snapshot_remove_device(&msg->device_uid);
}
