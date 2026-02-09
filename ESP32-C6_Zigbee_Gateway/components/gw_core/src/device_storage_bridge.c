#include "gw_core/device_storage_bridge.h"
#include "gw_core/zb_model.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "gw_device_bridge";

esp_err_t gw_device_storage_bridge_init(void)
{
    esp_err_t err = gw_device_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device storage: %s", esp_err_to_name(err));
        return err;
    }
    // C6 topology source is live zb_model (from Zigbee discovery), not persisted endpoints.
    ESP_LOGI(TAG, "Device storage bridge initialized (live topology mode)");
    return ESP_OK;
}

esp_err_t gw_device_storage_sync_endpoints(const gw_device_uid_t *uid)
{
    (void)uid;
    // No-op in live topology mode.
    return ESP_OK;
}

esp_err_t gw_device_storage_load_endpoints_to_zb_model(const gw_device_uid_t *uid)
{
    if (!uid) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_device_endpoint_t endpoints[GW_DEVICE_MAX_ENDPOINTS];
    size_t count = gw_device_storage_get_endpoints(uid, endpoints, GW_DEVICE_MAX_ENDPOINTS);
    
    if (count == 0) {
        ESP_LOGD(TAG, "No persistent endpoints found for device %s", uid->uid);
        return ESP_OK;
    }

    // Get device short address for zb_model
    gw_device_full_t device;
    esp_err_t err = gw_device_storage_get(uid, &device);
    if (err != ESP_OK) {
        return err;
    }

    // Convert and add to zb_model
    for (size_t i = 0; i < count; i++) {
        gw_zb_endpoint_t zb_ep = {0};
        zb_ep.uid = device.device_uid;
        zb_ep.short_addr = device.short_addr;
        zb_ep.endpoint = i + 1; // Note: endpoint number should be stored in device_endpoint_t
        zb_ep.profile_id = endpoints[i].profile_id;
        zb_ep.device_id = endpoints[i].device_id;
        zb_ep.in_cluster_count = endpoints[i].in_cluster_count;
        zb_ep.out_cluster_count = endpoints[i].out_cluster_count;
        
        memcpy(zb_ep.in_clusters, endpoints[i].in_clusters, 
               endpoints[i].in_cluster_count * sizeof(uint16_t));
        memcpy(zb_ep.out_clusters, endpoints[i].out_clusters, 
               endpoints[i].out_cluster_count * sizeof(uint16_t));
        
        err = gw_zb_model_upsert_endpoint(&zb_ep);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add endpoint to zb_model for device %s: %s", 
                     uid->uid, esp_err_to_name(err));
        }
    }
    
    ESP_LOGI(TAG, "Loaded %zu endpoints from persistent storage to zb_model for device %s", count, uid->uid);
    return ESP_OK;
}

size_t gw_device_storage_get_zb_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return gw_zb_model_list_endpoints(uid, out_eps, max_eps);
}
