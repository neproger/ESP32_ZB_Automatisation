#include "gw_core/device_storage_bridge.h"
#include "gw_core/zb_model.h"

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

size_t gw_device_storage_get_zb_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return gw_zb_model_list_endpoints(uid, out_eps, max_eps);
}
