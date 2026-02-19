#include "gw_cloud_sync.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "gw_core/event_bus.h"
#include "gw_core/net_time.h"
#include "gw_core/weather.h"
#include "gw_wifi_sta.h"

static const char *TAG = "gw_cloud";

static const double kWeatherLat = 43.238949;
static const double kWeatherLon = 76.889709;
static const uint32_t kWeatherIntervalMs = 60u * 60u * 1000u;
static const uint32_t kTimeIntervalMs = 6u * 60u * 60u * 1000u;

static bool s_started = false;
static bool s_cloud_initialized = false;
static char s_wifi_ssid[33];
static char s_wifi_password[65];

esp_err_t gw_cloud_sync_start_net_services(void);

static void publish_time_sync(uint64_t epoch_ms)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "epoch_ms=%" PRIu64, epoch_ms);
    gw_event_bus_publish_zb("system.time_sync", "cloud", "", 0, msg, 0, "", 0, 0,
                            GW_EVENT_VALUE_I64, false, (int64_t)epoch_ms, 0.0, NULL, NULL, 0);
}

static void publish_weather(const gw_weather_snapshot_t *snap)
{
    char msg[48];

    snprintf(msg, sizeof(msg), "temperature_c=%.1f", (double)snap->temperature_c);
    gw_event_bus_publish_zb("system.weather_temp_c", "cloud", "", 0, msg, 0, "", 0, 0,
                            GW_EVENT_VALUE_F64, false, 0, (double)snap->temperature_c, NULL, NULL, 0);

    snprintf(msg, sizeof(msg), "humidity_pct=%.1f", (double)snap->humidity_pct);
    gw_event_bus_publish_zb("system.weather_humidity_pct", "cloud", "", 0, msg, 0, "", 0, 0,
                            GW_EVENT_VALUE_F64, false, 0, (double)snap->humidity_pct, NULL, NULL, 0);

    snprintf(msg, sizeof(msg), "wind_kmh=%.1f", (double)snap->wind_speed_kmh);
    gw_event_bus_publish_zb("system.weather_wind_kmh", "cloud", "", 0, msg, 0, "", 0, 0,
                            GW_EVENT_VALUE_F64, false, 0, (double)snap->wind_speed_kmh, NULL, NULL, 0);

    snprintf(msg, sizeof(msg), "weather_code=%d", snap->weather_code);
    gw_event_bus_publish_zb("system.weather_code", "cloud", "", 0, msg, 0, "", 0, 0,
                            GW_EVENT_VALUE_I64, false, (int64_t)snap->weather_code, 0.0, NULL, NULL, 0);
}

static void cloud_pub_task(void *arg)
{
    (void)arg;
    uint64_t last_sync_ms = 0;
    uint64_t last_weather_mono = 0;
    uint32_t unsynced_retry_ticks = 0;
    uint32_t weather_retry_ticks = 0;

    for (;;) {
        if (!gw_wifi_sta_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        uint64_t sync_ms = gw_net_time_last_sync_ms();
        if (sync_ms > 0 && sync_ms != last_sync_ms) {
            last_sync_ms = sync_ms;
            publish_time_sync(sync_ms);
            ESP_LOGI(TAG, "published time sync");
            unsynced_retry_ticks = 0;
        } else if (!gw_net_time_is_synced()) {
            unsynced_retry_ticks++;
            if (unsynced_retry_ticks >= 15) { // ~30s (loop is 2s)
                (void)gw_net_time_request_sync();
                unsynced_retry_ticks = 0;
            }
        }

        gw_weather_snapshot_t snap = {0};
        if (gw_weather_get_snapshot(&snap) == ESP_OK && snap.valid && snap.updated_mono_ms != last_weather_mono) {
            last_weather_mono = snap.updated_mono_ms;
            publish_weather(&snap);
            ESP_LOGI(TAG, "published weather update");
            weather_retry_ticks = 0;
        } else if (!gw_weather_is_ready()) {
            weather_retry_ticks++;
            if (weather_retry_ticks >= 15) { // ~30s
                (void)gw_weather_request_refresh();
                weather_retry_ticks = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t gw_cloud_sync_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    if (xTaskCreateWithCaps(cloud_pub_task, "gw_cloud_pub", 4096, NULL, 2, NULL, MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    gw_event_bus_publish("system.cloud_wait_wifi", "cloud", "", 0, "cloud client waiting for wifi credentials");
    return ESP_OK;
}

esp_err_t gw_cloud_sync_set_wifi_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid));
    strlcpy(s_wifi_password, password, sizeof(s_wifi_password));

    if (s_cloud_initialized) {
        (void)gw_cloud_sync_start_net_services();
        return ESP_OK;
    }

    esp_err_t err = gw_wifi_sta_start(s_wifi_ssid, s_wifi_password);
    if (err != ESP_OK) {
        return err;
    }

    const gw_net_time_cfg_t tcfg = {
        .ntp_server = "pool.ntp.org",
        .sync_interval_ms = kTimeIntervalMs,
        .sync_timeout_ms = 8000,
        .sync_on_init = true,
    };
    err = gw_net_time_init(&tcfg);
    if (err != ESP_OK) {
        return err;
    }

    const gw_weather_cfg_t wcfg = {
        .latitude = kWeatherLat,
        .longitude = kWeatherLon,
        .refresh_interval_ms = kWeatherIntervalMs,
        .request_timeout_ms = 8000,
        .refresh_on_init = true,
    };
    err = gw_weather_init(&wcfg);
    if (err != ESP_OK) {
        return err;
    }

    s_cloud_initialized = true;
    (void)gw_cloud_sync_start_net_services();
    gw_event_bus_publish("system.cloud_ready", "cloud", "", 0, "time/weather client started");
    return ESP_OK;
}

esp_err_t gw_cloud_sync_start_net_services(void)
{
    if (!s_cloud_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!gw_wifi_sta_is_connected()) {
        ESP_LOGW(TAG, "net services kick skipped: wifi has no IP yet");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t t_err = gw_net_time_request_sync();
    if (t_err != ESP_OK && t_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "net time request failed: %s", esp_err_to_name(t_err));
    }

    esp_err_t w_err = gw_weather_request_refresh();
    if (w_err != ESP_OK && w_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "weather request failed: %s", esp_err_to_name(w_err));
    }

    ESP_LOGI(TAG, "net services kick requested");
    return (t_err == ESP_OK || w_err == ESP_OK) ? ESP_OK : ESP_FAIL;
}
