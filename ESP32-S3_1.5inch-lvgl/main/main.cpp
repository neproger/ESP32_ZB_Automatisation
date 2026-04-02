#include "devices_init.h"
#include "ui/ui_app.hpp"

#include "gw_wifi.h"
#include "gw_http/gw_http.h"
#include "s3_weather_service.h"
#include "gw_core/automation_store.h"
#include "gw_core/gw_proto_bus.h"
#include "gw_core/project_settings.h"
#include "gw_core/sensor_store.h"
#include "gw_core/rules_engine.h"
#include "gw_core/net_time.h"
#include "gw_core/gw_proto.h"
#include "gw_zigbee/gw_zigbee.h"
#include "gw_proto/gw_proto_frame.h"
#include "gw_proto/gw_proto_map.h"
#include "gw_model/gw_model.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG_APP = "s3_backend";
static constexpr bool kEnableHttpServer = true;
static constexpr uint32_t kUiBootTaskStack = 12288;
static constexpr bool kEnableZigbeeRuntime = true;
static constexpr bool kEnableWeatherRuntime = true;
static constexpr bool kEnableWifiRuntime = true;
static bool s_http_started = false;
static volatile bool s_ui_ready_for_http = false;

static void log_struct_sizes_once(void)
{
    ESP_LOGI(TAG_APP,
             "Struct sizes: automation=%u device=%u endpoint=%u state=%u group=%u group_item=%u settings=%u proto_frame_max=%u",
             (unsigned)sizeof(gw_automation_entry_t),
             (unsigned)sizeof(gw_proto_device_v1_t),
             (unsigned)sizeof(gw_proto_endpoint_v1_t),
             (unsigned)sizeof(gw_proto_state_item_v1_t),
             (unsigned)sizeof(gw_proto_group_v1_t),
             (unsigned)sizeof(gw_proto_group_item_v1_t),
             (unsigned)sizeof(gw_proto_settings_v1_t),
             (unsigned)GW_PROTO_FRAME_MAX_SIZE);
}

static bool http_has_memory_headroom(void)
{
    return true;
}

static void http_start_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (kEnableHttpServer && s_ui_ready_for_http && !s_http_started) {
            if (!http_has_memory_headroom()) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            esp_err_t http_err = gw_http_start();
            if (http_err != ESP_OK) {
                ESP_LOGW(TAG_APP, "HTTP start failed (%s), retry in 10s", esp_err_to_name(http_err));
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            s_http_started = true;
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void ui_boot_task(void *arg)
{
    (void)arg;

    esp_err_t err = devices_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_APP, "Devices init failed: %s", esp_err_to_name(err));
    } else {
        ui_app_init();
        s_ui_ready_for_http = true;
        ESP_LOGI(TAG_APP, "UI started");
    }
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(gw_proto_bus_init());
    log_struct_sizes_once();
    // Temporary model-first bringup: boot UI on gw_model without legacy stores.
    esp_log_level_set("gw_zigbee_uart", ESP_LOG_INFO);
    esp_log_level_set("gw_event", ESP_LOG_WARN);
    esp_log_level_set("gw_runtime_sync", ESP_LOG_WARN);
    esp_log_level_set("gw_state_store", ESP_LOG_WARN);
    esp_log_level_set("s3_weather_svc", ESP_LOG_WARN);
    esp_log_level_set("s3_weather_http", ESP_LOG_WARN);
    esp_log_level_set("gw_net_time", ESP_LOG_WARN);

    ESP_ERROR_CHECK(gw_sensor_store_init());
    ESP_ERROR_CHECK(gw_model_init());
    ESP_ERROR_CHECK(gw_automation_store_init());
    ESP_ERROR_CHECK(gw_project_settings_init());
    ESP_ERROR_CHECK(gw_rules_init());

    ESP_LOGW(TAG_APP, "Legacy stores and legacy runtime are disabled for gw_model bringup");

    if (kEnableZigbeeRuntime) {
        esp_err_t zb_link_err = gw_zigbee_link_start();
        if (zb_link_err != ESP_OK) {
            ESP_LOGW(TAG_APP, "Zigbee UART link start failed (%s)", esp_err_to_name(zb_link_err));
        } else {
            ESP_LOGI(TAG_APP, "Zigbee ingress path started: UART -> INGRESS -> gw_model");
        }
    }

    if (kEnableWifiRuntime) {
        ESP_ERROR_CHECK(gw_net_time_init(NULL));
        esp_err_t wifi_boot_err = gw_wifi_start();
        if (wifi_boot_err != ESP_OK) {
            ESP_LOGW(TAG_APP, "Wi-Fi service start failed (%s)", esp_err_to_name(wifi_boot_err));
        }
    }

    if (kEnableWeatherRuntime) {
        ESP_ERROR_CHECK(s3_weather_service_start());
    }

    // HTTP start may mount SPIFFS (flash operations). Stack must be internal RAM.
    if (kEnableHttpServer &&
        xTaskCreateWithCaps(http_start_task, "http_start", 4096, NULL, 3, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG_APP, "http_start task create failed");
    }
    if (kEnableHttpServer) {
        ESP_LOGI(TAG_APP, "HTTP start deferred until UI init completes");
    } else {
        ESP_LOGW(TAG_APP, "HTTP/WS disabled for model-first bringup");
    }

    // Bring up display/LVGL/UI at the end on a dedicated internal-RAM stack.
    if (xTaskCreateWithCaps(ui_boot_task, "ui_boot", kUiBootTaskStack, NULL, 4, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG_APP, "ui_boot task create failed");
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
