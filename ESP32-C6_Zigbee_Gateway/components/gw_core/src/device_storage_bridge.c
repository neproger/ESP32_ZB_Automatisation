#include "gw_core/device_storage_bridge.h"
#include "gw_core/zb_model.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "gw_device_bridge";

static bool slot_has_endpoint_payload(const gw_device_endpoint_t *ep)
{
    if (!ep) {
        return false;
    }
    return ep->profile_id != 0 ||
           ep->device_id != 0 ||
           ep->in_cluster_count != 0 ||
           ep->out_cluster_count != 0;
}

esp_err_t gw_device_storage_bridge_init(void)
{
    esp_err_t err = gw_device_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device storage: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Device storage bridge initialized (storage topology mode)");
    return ESP_OK;
}

esp_err_t gw_device_storage_sync_endpoints(const gw_device_uid_t *uid)
{
    if (!uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_full_t d = {0};
    esp_err_t err = gw_device_storage_get(uid, &d);
    if (err != ESP_OK) {
        return err;
    }

    gw_zb_endpoint_t live_eps[GW_DEVICE_MAX_ENDPOINTS] = {0};
    size_t live_count = gw_zb_model_list_endpoints(uid, live_eps, GW_DEVICE_MAX_ENDPOINTS);
    if (live_count == 0) {
        // Keep persisted endpoints when live model is empty (cold boot/offline nodes).
        return ESP_OK;
    }

    memset(d.endpoints, 0, sizeof(d.endpoints));
    d.endpoint_count = 0;
    d.has_onoff = false;

    for (size_t i = 0; i < live_count; i++) {
        const gw_zb_endpoint_t *src = &live_eps[i];
        if (src->endpoint == 0 || src->endpoint > GW_DEVICE_MAX_ENDPOINTS) {
            continue;
        }
        size_t slot = (size_t)(src->endpoint - 1);
        gw_device_endpoint_t *dst = &d.endpoints[slot];
        dst->profile_id = src->profile_id;
        dst->device_id = src->device_id;
        dst->in_cluster_count = src->in_cluster_count > GW_DEVICE_MAX_CLUSTERS ? GW_DEVICE_MAX_CLUSTERS : src->in_cluster_count;
        dst->out_cluster_count = src->out_cluster_count > GW_DEVICE_MAX_CLUSTERS ? GW_DEVICE_MAX_CLUSTERS : src->out_cluster_count;
        if (dst->in_cluster_count > 0) {
            memcpy(dst->in_clusters, src->in_clusters, dst->in_cluster_count * sizeof(uint16_t));
        }
        if (dst->out_cluster_count > 0) {
            memcpy(dst->out_clusters, src->out_clusters, dst->out_cluster_count * sizeof(uint16_t));
        }
        if (src->endpoint > d.endpoint_count) {
            d.endpoint_count = src->endpoint;
        }
        for (size_t ci = 0; ci < dst->in_cluster_count; ci++) {
            if (dst->in_clusters[ci] == 0x0006) {
                d.has_onoff = true;
                break;
            }
        }
    }

    return gw_device_storage_upsert(&d);
}

size_t gw_device_storage_get_zb_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    if (!uid || !out_eps || max_eps == 0) {
        return 0;
    }
    gw_device_full_t d = {0};
    if (gw_device_storage_get(uid, &d) != ESP_OK) {
        return 0;
    }

    size_t out_count = 0;
    size_t max_slots = d.endpoint_count > GW_DEVICE_MAX_ENDPOINTS ? GW_DEVICE_MAX_ENDPOINTS : d.endpoint_count;
    for (size_t slot = 0; slot < max_slots && out_count < max_eps; slot++) {
        const gw_device_endpoint_t *src = &d.endpoints[slot];
        if (!slot_has_endpoint_payload(src)) {
            continue;
        }
        gw_zb_endpoint_t *dst = &out_eps[out_count++];
        memset(dst, 0, sizeof(*dst));
        dst->uid = d.device_uid;
        dst->short_addr = d.short_addr;
        dst->endpoint = (uint8_t)(slot + 1);
        dst->profile_id = src->profile_id;
        dst->device_id = src->device_id;
        dst->in_cluster_count = src->in_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : src->in_cluster_count;
        dst->out_cluster_count = src->out_cluster_count > GW_ZB_MAX_CLUSTERS ? GW_ZB_MAX_CLUSTERS : src->out_cluster_count;
        if (dst->in_cluster_count > 0) {
            memcpy(dst->in_clusters, src->in_clusters, dst->in_cluster_count * sizeof(uint16_t));
        }
        if (dst->out_cluster_count > 0) {
            memcpy(dst->out_clusters, src->out_clusters, dst->out_cluster_count * sizeof(uint16_t));
        }
    }
    return out_count;
}
