#include "devices_init.h"
#include "ui/ui_app.hpp"

#include "gw_wifi.h"
#include "gw_http/gw_http.h"
#include "gw_core/event_bus.h"
#include "gw_core/device_registry.h"
#include "gw_core/device_fb_store.h"
#include "gw_core/automation_store.h"
#include "gw_core/group_store.h"
#include "gw_core/sensor_store.h"
#include "gw_core/state_store.h"
#include "gw_core/rules_engine.h"
#include "gw_core/runtime_sync.h"
#include "gw_core/zb_model.h"
#include "gw_zigbee/gw_zigbee.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG_APP = "s3_backend";
static constexpr bool kEnableHttpServer = true;
static bool s_http_started = false;

static void log_heap_caps(const char *stage)
{
    ESP_LOGI(TAG_APP,
             "Heap %s: internal=%u (largest=%u) dma=%u (largest=%u) psram=%u (largest=%u)",
             stage ? stage : "-",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static bool wifi_is_connected(void)
{
    wifi_ap_record_t ap{};
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
}

static void wifi_connect_task(void *arg)
{
    (void)arg;
    bool ps_configured = false;
    for (;;) {
        if (!wifi_is_connected()) {
            esp_err_t wifi_err = gw_wifi_connect_multi();
            if (wifi_err != ESP_OK) {
                ESP_LOGW(TAG_APP, "Wi-Fi reconnect attempt failed (%s), retry in 10s", esp_err_to_name(wifi_err));
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            ps_configured = false;
        }

        if (kEnableHttpServer && !s_http_started) {
            log_heap_caps("before_http_start");
            esp_err_t http_err = gw_http_start();
            if (http_err != ESP_OK) {
                ESP_LOGW(TAG_APP, "HTTP start failed (%s), retry in 10s", esp_err_to_name(http_err));
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            s_http_started = true;
            log_heap_caps("after_http_start");
        }

        if (!ps_configured) {
            (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            ps_configured = true;
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

extern "C" void app_main(void)
{
    log_heap_caps("boot_entry");
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(gw_event_bus_init());
    // Temporary log noise reduction for bring-up/debug sessions.
    esp_log_level_set("gw_zigbee_uart", ESP_LOG_WARN);
    esp_log_level_set("gw_event", ESP_LOG_WARN);
    esp_log_level_set("gw_state_store", ESP_LOG_INFO);

    ESP_ERROR_CHECK(gw_zb_model_init());
    ESP_ERROR_CHECK(gw_sensor_store_init());
    ESP_ERROR_CHECK(gw_state_store_init());
    ESP_ERROR_CHECK(gw_device_registry_init());
    ESP_ERROR_CHECK(gw_device_fb_store_init());
    ESP_ERROR_CHECK(gw_automation_store_init());
    ESP_ERROR_CHECK(gw_group_store_init());
    ESP_ERROR_CHECK(gw_rules_init());
    ESP_ERROR_CHECK(gw_runtime_sync_init());
    log_heap_caps("after_core_init");

    // Start Zigbee UART backend before display/UI to prioritize Wi-Fi and HTTP bring-up.
    esp_err_t zb_link_err = gw_zigbee_link_start();
    if (zb_link_err != ESP_OK) {
        ESP_LOGW(TAG_APP, "Zigbee UART link start failed (%s)", esp_err_to_name(zb_link_err));
    }
    log_heap_caps("after_zigbee_link_start");

    // Start Wi-Fi before display/UI to reserve Wi-Fi internal resources first.
    esp_err_t wifi_boot_err = gw_wifi_connect_multi();
    if (wifi_boot_err != ESP_OK) {
        ESP_LOGW(TAG_APP, "Initial Wi-Fi connect failed (%s)", esp_err_to_name(wifi_boot_err));
    } else if (kEnableHttpServer && !s_http_started) {
        log_heap_caps("before_http_start");
        esp_err_t http_err = gw_http_start();
        if (http_err != ESP_OK) {
            ESP_LOGW(TAG_APP, "HTTP start failed (%s), will retry in wifi task", esp_err_to_name(http_err));
        } else {
            s_http_started = true;
            log_heap_caps("after_http_start");
        }
    }

    if (xTaskCreateWithCaps(wifi_connect_task, "wifi_connect", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG_APP, "wifi_connect task create failed");
    }
    log_heap_caps("after_wifi_task_create");

    if (!kEnableHttpServer) {
        ESP_LOGW(TAG_APP, "HTTP/WS disabled for UI stability test");
    }

    // Bring up display/LVGL/UI at the end.
    esp_err_t err = devices_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_APP, "Devices init failed: %s", esp_err_to_name(err));
    } else {
        ui_app_init();
        ESP_LOGI(TAG_APP, "UI started");
    }
    log_heap_caps("after_devices_ui_init");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
