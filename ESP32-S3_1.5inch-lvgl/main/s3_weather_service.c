#include "s3_weather_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "s3_geoip_http.h"
#include "s3_weather_http.h"
#include "gw_core/net_time.h"
#include "gw_core/state_store.h"

static const char *TAG = "s3_weather_svc";

static const double kFallbackLat = 43.238949;
static const double kFallbackLon = 76.889709;
static const TickType_t kRetryTicks = pdMS_TO_TICKS(10000);
static const TickType_t kSuccessTicks = pdMS_TO_TICKS(60 * 60 * 1000);

static TaskHandle_t s_task;
static bool s_started;
static bool s_geo_ready;
static double s_geo_lat = kFallbackLat;
static double s_geo_lon = kFallbackLon;
static char s_location[64] = "Locating...";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool wifi_is_connected_local(void)
{
    wifi_ap_record_t ap = {0};
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
}

static uint64_t now_ts_ms(void)
{
    uint64_t ts = gw_net_time_now_ms();
    if (ts != 0) {
        return ts;
    }
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static void set_location_text(const char *text)
{
    if (!text) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_location, text, sizeof(s_location));
    portEXIT_CRITICAL(&s_lock);
}

void s3_weather_service_get_location(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(out, s_location, out_size);
    portEXIT_CRITICAL(&s_lock);
}

static void persist_geo_to_state_store(double lat, double lon)
{
    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, "0xWEATHER000000001", sizeof(uid.uid));
    const uint8_t endpoint = 1;
    const uint64_t ts_ms = now_ts_ms();
    (void)gw_state_store_set_f32(&uid, endpoint, "weather_lat", (float)lat, ts_ms);
    (void)gw_state_store_set_f32(&uid, endpoint, "weather_lon", (float)lon, ts_ms);
}

static esp_err_t ensure_geo_location(void)
{
    if (s_geo_ready) {
        return ESP_OK;
    }

    s3_geoip_result_t geo = {0};
    char err[128] = {0};
    esp_err_t geo_err = s3_geoip_http_fetch_once(8000, &geo, err, sizeof(err));
    if (geo_err != ESP_OK || !geo.valid) {
        ESP_LOGW(TAG, "geoip failed: err=%s detail=%s", esp_err_to_name(geo_err), err[0] ? err : "-");
        set_location_text("Location unknown");
        return ESP_FAIL;
    }

    s_geo_lat = geo.latitude;
    s_geo_lon = geo.longitude;
    s_geo_ready = true;

    char loc[64] = {0};
    if (geo.city[0] && geo.region[0]) {
        (void)snprintf(loc, sizeof(loc), "%s, %s", geo.city, geo.region);
    } else if (geo.city[0]) {
        strlcpy(loc, geo.city, sizeof(loc));
    } else if (geo.region[0]) {
        strlcpy(loc, geo.region, sizeof(loc));
    } else {
        strlcpy(loc, "Unknown location", sizeof(loc));
    }
    set_location_text(loc);
    persist_geo_to_state_store(s_geo_lat, s_geo_lon);
    ESP_LOGI(TAG, "geoip resolved: %s (lat=%.6f lon=%.6f)", loc, s_geo_lat, s_geo_lon);
    return ESP_OK;
}

static void persist_weather_to_state_store(const s3_weather_result_t *res)
{
    if (!res || !res->valid) {
        return;
    }

    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, "0xWEATHER000000001", sizeof(uid.uid));
    const uint8_t endpoint = 1;
    const uint64_t ts_ms = now_ts_ms();

    (void)gw_state_store_set_f32(&uid, endpoint, "weather_temp_c", res->temperature_c, ts_ms);
    (void)gw_state_store_set_f32(&uid, endpoint, "weather_humidity_pct", res->humidity_pct, ts_ms);
    (void)gw_state_store_set_f32(&uid, endpoint, "weather_wind_kmh", res->wind_speed_kmh, ts_ms);
    (void)gw_state_store_set_u32(&uid, endpoint, "weather_code", (uint32_t)res->weather_code, ts_ms);
    (void)gw_state_store_set_u64(&uid, endpoint, "weather_updated_ms", ts_ms, ts_ms);
}

static void weather_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (!wifi_is_connected_local()) {
            ESP_LOGW(TAG, "weather fetch skipped: Wi-Fi not connected");
            set_location_text("Wi-Fi disconnected");
            vTaskDelay(kRetryTicks);
            continue;
        }

        if (ensure_geo_location() != ESP_OK) {
            vTaskDelay(kRetryTicks);
            continue;
        }

        s3_weather_result_t res = {0};
        char err[128] = {0};
        esp_err_t fetch_err = s3_weather_http_fetch_once(s_geo_lat, s_geo_lon, 8000, &res, err, sizeof(err));
        if (fetch_err != ESP_OK || !res.valid) {
            ESP_LOGW(TAG, "weather fetch failed: err=%s detail=%s",
                     esp_err_to_name(fetch_err), err[0] ? err : "-");
            vTaskDelay(kRetryTicks);
            continue;
        }

        persist_weather_to_state_store(&res);
        ESP_LOGI(TAG,
                 "weather updated: t=%.1fC h=%.1f%% wind=%.1fkm/h code=%d obs=%s",
                 (double)res.temperature_c,
                 (double)res.humidity_pct,
                 (double)res.wind_speed_kmh,
                 res.weather_code,
                 res.observed_time);
        vTaskDelay(kSuccessTicks);
    }
}

esp_err_t s3_weather_service_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreateWithCaps(
        weather_task,
        "s3_weather",
        6144,
        NULL,
        3,
        &s_task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}
