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
#include "gw_core/net_time.h"
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
#include <cstring>
#include <cstdio>

static const char *TAG_APP = "s3_backend";
static constexpr bool kEnableHttpServer = true;
static bool s_http_started = false;
static volatile bool s_ui_ready_for_http = false;

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

static bool http_has_memory_headroom(void)
{
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    // Pragmatic gate for tight-memory profile:
    // - keep a small internal margin for httpd task/handlers
    // - DMA can be low because LVGL/display already occupies most of it
    return (free_internal >= 8 * 1024) &&
           (largest_internal >= 5 * 1024) &&
           (free_dma >= 1024) &&
           (largest_dma >= 768);
}

static void wifi_connect_task(void *arg)
{
    (void)arg;
    bool ps_configured = false;
    bool c6_wifi_synced = false;
    char last_ssid[33] = {0};
    char last_pass[65] = {0};
    for (;;) {
        if (!wifi_is_connected()) {
            esp_err_t wifi_err = gw_wifi_connect_multi();
            if (wifi_err != ESP_OK) {
                ESP_LOGW(TAG_APP, "Wi-Fi reconnect attempt failed (%s), retry in 10s", esp_err_to_name(wifi_err));
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            ps_configured = false;
            c6_wifi_synced = false;
        }

        if (!c6_wifi_synced) {
            wifi_config_t wifi_cfg = {};
            if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
                const char *ssid = (const char *)wifi_cfg.sta.ssid;
                const char *pass = (const char *)wifi_cfg.sta.password;
                if (ssid[0] != '\0') {
                    const bool changed = (strcmp(last_ssid, ssid) != 0) || (strcmp(last_pass, pass) != 0);
                    if (changed) {
                        esp_err_t sync_err = gw_zigbee_set_c6_wifi_credentials(ssid, pass);
                        if (sync_err != ESP_OK) {
                            ESP_LOGW(TAG_APP, "C6 Wi-Fi creds sync failed (%s)", esp_err_to_name(sync_err));
                        } else {
                            snprintf(last_ssid, sizeof(last_ssid), "%s", ssid);
                            snprintf(last_pass, sizeof(last_pass), "%s", pass);
                            ESP_LOGI(TAG_APP, "C6 Wi-Fi credentials synced");
                        }
                    }
                    c6_wifi_synced = true;
                }
            }
        }

        if (kEnableHttpServer && s_ui_ready_for_http && !s_http_started) {
            if (!http_has_memory_headroom()) {
                ESP_LOGW(TAG_APP,
                         "HTTP start deferred: internal=%u largest=%u dma=%u largest_dma=%u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
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
    ESP_ERROR_CHECK(gw_net_time_init(NULL));
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
    }

    // This task may mount SPIFFS (flash operations). Stack must be internal RAM.
    if (xTaskCreateWithCaps(wifi_connect_task, "wifi_connect", 6144, NULL, 3, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG_APP, "wifi_connect task create failed");
    }
    log_heap_caps("after_wifi_task_create");
    if (kEnableHttpServer) {
        ESP_LOGI(TAG_APP, "HTTP start deferred until UI init completes");
    }

    if (!kEnableHttpServer) {
        ESP_LOGW(TAG_APP, "HTTP/WS disabled for UI stability test");
    }

    // Bring up display/LVGL/UI at the end.
    esp_err_t err = devices_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_APP, "Devices init failed: %s", esp_err_to_name(err));
    } else {
        ui_app_init();
        s_ui_ready_for_http = true;
        ESP_LOGI(TAG_APP, "UI started");
    }
    log_heap_caps("after_devices_ui_init");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
