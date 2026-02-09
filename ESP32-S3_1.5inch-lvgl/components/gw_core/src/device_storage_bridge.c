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

    // Load all existing devices into zb_model for compatibility
    static gw_device_full_t devices[32];
    size_t count = gw_device_storage_list(devices, 32);
    
    for (size_t i = 0; i < count; i++) {
        gw_device_storage_load_endpoints_to_zb_model(&devices[i].device_uid);
    }
    
    ESP_LOGI(TAG, "Device storage bridge initialized, loaded %zu devices", count);
    return ESP_OK;
}

esp_err_t gw_device_storage_sync_endpoints(const gw_device_uid_t *uid)
{
    if (!uid) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get endpoints from zb_model
    gw_zb_endpoint_t zb_eps[GW_DEVICE_MAX_ENDPOINTS];
    size_t count = gw_zb_model_list_endpoints(uid, zb_eps, GW_DEVICE_MAX_ENDPOINTS);
    
    if (count == 0) {
        ESP_LOGD(TAG, "No endpoints found in zb_model for device %s", uid->uid);
        return ESP_OK;
    }

    // Get current device to update
    gw_device_full_t device;
    esp_err_t err = gw_device_storage_get(uid, &device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get device %s from storage: %s", uid->uid, esp_err_to_name(err));
        return err;
    }

    // Clear existing endpoints and add new ones
    device.endpoint_count = 0;
    memset(device.endpoints, 0, sizeof(device.endpoints));
    
    for (size_t i = 0; i < count && i < GW_DEVICE_MAX_ENDPOINTS; i++) {
        gw_device_endpoint_t *ep = &device.endpoints[i];
        
        ep->profile_id = zb_eps[i].profile_id;
        ep->device_id = zb_eps[i].device_id;
        ep->in_cluster_count = zb_eps[i].in_cluster_count;
        ep->out_cluster_count = zb_eps[i].out_cluster_count;
        
        // Copy clusters (truncated if needed)
        size_t in_count = zb_eps[i].in_cluster_count < GW_DEVICE_MAX_CLUSTERS ? 
                         zb_eps[i].in_cluster_count : GW_DEVICE_MAX_CLUSTERS;
        memcpy(ep->in_clusters, zb_eps[i].in_clusters, in_count * sizeof(uint16_t));
        ep->in_cluster_count = in_count;
        
        size_t out_count = zb_eps[i].out_cluster_count < GW_DEVICE_MAX_CLUSTERS ? 
                          zb_eps[i].out_cluster_count : GW_DEVICE_MAX_CLUSTERS;
        memcpy(ep->out_clusters, zb_eps[i].out_clusters, out_count * sizeof(uint16_t));
        ep->out_cluster_count = out_count;
        
        device.endpoint_count++;
    }
    
    // Save back to storage
    err = gw_device_storage_upsert(&device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device %s with endpoints: %s", uid->uid, esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Synced %zu endpoints for device %s to persistent storage", device.endpoint_count, uid->uid);
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
    if (!uid || !out_eps || max_eps == 0) {
        return 0;
    }

    // Get device to get short address
    gw_device_full_t device;
    esp_err_t err = gw_device_storage_get(uid, &device);
    if (err != ESP_OK) {
        return 0;
    }

    gw_device_endpoint_t endpoints[GW_DEVICE_MAX_ENDPOINTS];
    size_t count = gw_device_storage_get_endpoints(uid, endpoints, GW_DEVICE_MAX_ENDPOINTS);
    
    size_t result_count = count < max_eps ? count : max_eps;
    for (size_t i = 0; i < result_count; i++) {
        out_eps[i].uid = device.device_uid;
        out_eps[i].short_addr = device.short_addr;
        out_eps[i].endpoint = i + 1; // Note: endpoint number should be stored in device_endpoint_t
        out_eps[i].profile_id = endpoints[i].profile_id;
        out_eps[i].device_id = endpoints[i].device_id;
        out_eps[i].in_cluster_count = endpoints[i].in_cluster_count;
        out_eps[i].out_cluster_count = endpoints[i].out_cluster_count;
        
        memcpy(out_eps[i].in_clusters, endpoints[i].in_clusters, 
               endpoints[i].in_cluster_count * sizeof(uint16_t));
        memcpy(out_eps[i].out_clusters, endpoints[i].out_clusters, 
               endpoints[i].out_cluster_count * sizeof(uint16_t));
    }
    
    return result_count;
}