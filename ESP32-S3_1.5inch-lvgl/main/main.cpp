#include "devices_init.h"
#include "ui/ui_app.hpp"

#include "gw_wifi.h"
#include "gw_http/gw_http.h"
#include "gw_core/event_bus.h"
#include "gw_core/device_registry.h"
#include "gw_core/device_fb_store.h"
#include "gw_core/automation_store.h"
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
#include "esp_timer.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG_APP = "s3_backend";
static bool s_ui_started = false;
static constexpr bool kEnableHttpServer = true;
static constexpr size_t kUiProbeMaxDevices = 64;
static gw_device_t s_ui_probe_devices[kUiProbeMaxDevices];

static uint32_t fnv1a_append_u8(uint32_t h, uint8_t v)
{
    h ^= (uint32_t)v;
    h *= 16777619u;
    return h;
}

static uint32_t fnv1a_append_u16(uint32_t h, uint16_t v)
{
    h = fnv1a_append_u8(h, (uint8_t)(v & 0xFFu));
    h = fnv1a_append_u8(h, (uint8_t)((v >> 8) & 0xFFu));
    return h;
}

static uint32_t ui_registry_signature(size_t *out_count)
{
    const size_t count = gw_device_registry_list(s_ui_probe_devices, kUiProbeMaxDevices);
    uint32_t h = 2166136261u;
    h = fnv1a_append_u16(h, (uint16_t)count);
    for (size_t i = 0; i < count; ++i) {
        const gw_device_t *d = &s_ui_probe_devices[i];
        for (size_t j = 0; j < sizeof(d->device_uid.uid); ++j) {
            h = fnv1a_append_u8(h, (uint8_t)d->device_uid.uid[j]);
        }
        h = fnv1a_append_u16(h, d->short_addr);
    }
    if (out_count) {
        *out_count = count;
    }
    return h;
}

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

static void ui_boot_task(void *arg)
{
    (void)arg;
    static constexpr int64_t kUiWaitTimeoutUs = 45LL * 1000LL * 1000LL;
    static constexpr int64_t kUiStableWindowUs = 2000LL * 1000LL;
    static constexpr int64_t kUiPollStepUs = 250LL * 1000LL;
    const int64_t t0 = esp_timer_get_time();

    while (!(gw_zigbee_bootstrap_ready() && gw_zigbee_state_warmup_ready())) {
        if ((esp_timer_get_time() - t0) >= kUiWaitTimeoutUs) {
            ESP_LOGW(TAG_APP, "UI warmup wait timeout, starting UI with current data");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Phase 2: wait for a short stabilization window in device registry.
    size_t stable_count = 0;
    uint32_t prev_sig = ui_registry_signature(&stable_count);
    int64_t stable_since = esp_timer_get_time();
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if ((now - t0) >= kUiWaitTimeoutUs) {
            ESP_LOGW(TAG_APP, "UI stabilization timeout, starting with current registry");
            break;
        }

        size_t current_count = 0;
        const uint32_t sig = ui_registry_signature(&current_count);
        if (sig != prev_sig) {
            prev_sig = sig;
            stable_count = current_count;
            stable_since = now;
        } else if ((now - stable_since) >= kUiStableWindowUs) {
            ESP_LOGI(TAG_APP,
                     "UI registry stabilized: devices=%u window_ms=%u",
                     (unsigned)stable_count,
                     (unsigned)(kUiStableWindowUs / 1000));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(kUiPollStepUs / 1000)));
    }

    if (!s_ui_started) {
        ui_app_init();
        s_ui_started = true;
        ESP_LOGI(TAG_APP, "UI started after bootstrap wait");
    }

    vTaskDelete(NULL);
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

        if (!ps_configured) {
            (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            ps_configured = true;
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void http_start_task(void *arg)
{
    (void)arg;
    static constexpr int64_t kUiWaitTimeoutUs = 30LL * 1000LL * 1000LL;
    static constexpr uint32_t kPostUiDelayMs = 1500;
    const int64_t t0 = esp_timer_get_time();

    while (!s_ui_started) {
        if ((esp_timer_get_time() - t0) >= kUiWaitTimeoutUs) {
            ESP_LOGW(TAG_APP, "HTTP start wait timeout, starting anyway");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    vTaskDelay(pdMS_TO_TICKS(kPostUiDelayMs));
    log_heap_caps("before_http_start");
    esp_err_t err = gw_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_APP, "HTTP start failed: %s", esp_err_to_name(err));
    } else {
        log_heap_caps("after_http_start");
    }

    vTaskDelete(NULL);
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

    // Bring up display/LVGL first while internal + DMA-capable heap is still mostly free.
    esp_err_t err = devices_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_APP, "Devices init failed: %s", esp_err_to_name(err));
    }
    log_heap_caps("after_devices_init");

    ESP_ERROR_CHECK(gw_zb_model_init());
    ESP_ERROR_CHECK(gw_sensor_store_init());
    ESP_ERROR_CHECK(gw_state_store_init());
    ESP_ERROR_CHECK(gw_device_registry_init());
    ESP_ERROR_CHECK(gw_device_fb_store_init());
    ESP_ERROR_CHECK(gw_automation_store_init());
    ESP_ERROR_CHECK(gw_rules_init());
    ESP_ERROR_CHECK(gw_runtime_sync_init());
    log_heap_caps("after_core_init");

    // Start network/backend after LVGL allocation to avoid DMA starvation on S3.
    xTaskCreateWithCaps(wifi_connect_task, "wifi_connect", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    log_heap_caps("after_wifi_task_create");

    esp_err_t zb_link_err = gw_zigbee_link_start();
    if (zb_link_err != ESP_OK) {
        ESP_LOGW(TAG_APP, "Zigbee UART link start failed (%s)", esp_err_to_name(zb_link_err));
    }
    log_heap_caps("after_zigbee_link_start");

    if (err == ESP_OK) {
        if (xTaskCreate(ui_boot_task, "ui_boot", 4096, NULL, 4, NULL) != pdPASS) {
            ESP_LOGW(TAG_APP, "ui_boot task create failed, starting UI immediately");
            ui_app_init();
            s_ui_started = true;
            ESP_LOGI(TAG_APP, "UI started (fallback)");
        }
    }

    if (kEnableHttpServer) {
        if (xTaskCreateWithCaps(http_start_task, "http_start", 4096, NULL, 3, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGW(TAG_APP, "http_start task create failed, starting HTTP inline");
            log_heap_caps("before_http_start");
            ESP_ERROR_CHECK(gw_http_start());
            log_heap_caps("after_http_start");
        }
    } else {
        ESP_LOGW(TAG_APP, "HTTP/WS disabled for UI stability test");
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


