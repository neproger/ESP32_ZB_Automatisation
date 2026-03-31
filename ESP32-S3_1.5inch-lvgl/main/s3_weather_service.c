#include "s3_weather_service.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "s3_geoip_http.h"
#include "s3_weather_http.h"
#include "gw_core/event_bus.h"
#include "gw_core/event_names.h"
#include "gw_core/net_time.h"
#include "gw_core/project_settings.h"
#include "gw_core/state_store.h"

static const char *TAG = "s3_weather_svc";

static const char *kWeatherUid = "0xWEATHER000000001";
static const uint8_t kWeatherEndpoint = 1;
static const uint64_t kGeoRefreshPeriodMs = 6ULL * 60ULL * 60ULL * 1000ULL;

static TaskHandle_t s_task;
static bool s_started;
static bool s_listener_registered;
static bool s_geo_ready;
static double s_geo_lat = 0.0;
static double s_geo_lon = 0.0;
static char s_geo_timezone[48] = {0};
static int32_t s_geo_offset_sec = 0;
static char s_location[64] = "Locating...";
static uint64_t s_last_geo_refresh_ms = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void persist_timezone_to_state_store(const char *tz_name);

static TickType_t ms_to_ticks_safe(uint32_t ms)
{
    const uint64_t ticks = ((uint64_t)ms * (uint64_t)configTICK_RATE_HZ) / 1000ULL;
    if (ticks == 0) {
        return (ms > 0) ? 1 : 0;
    }
    if (ticks > (uint64_t)portMAX_DELAY) {
        return portMAX_DELAY;
    }
    return (TickType_t)ticks;
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

static void apply_timezone_if_present(const char *tz_name)
{
    if (!tz_name || !tz_name[0]) {
        return;
    }
    if (setenv("TZ", tz_name, 1) == 0) {
        tzset();
        ESP_LOGI(TAG, "timezone applied: %s", tz_name);
    } else {
        ESP_LOGW(TAG, "timezone apply failed: %s", tz_name);
    }
}

static void apply_timezone_offset_if_present(int32_t offset_sec)
{
    if (offset_sec < -18 * 3600 || offset_sec > 18 * 3600) {
        return;
    }

    // POSIX TZ uses reversed sign: UTC+5 => "UTC-5"
    const int32_t abs_off = (offset_sec >= 0) ? offset_sec : -offset_sec;
    const int hours = (int)(abs_off / 3600);
    const int mins = (int)((abs_off % 3600) / 60);
    const char posix_sign = (offset_sec >= 0) ? '-' : '+';

    char tz_buf[24] = {0};
    if (mins == 0) {
        (void)snprintf(tz_buf, sizeof(tz_buf), "UTC%c%d", posix_sign, hours);
    } else {
        (void)snprintf(tz_buf, sizeof(tz_buf), "UTC%c%d:%02d", posix_sign, hours, mins);
    }

    if (setenv("TZ", tz_buf, 1) == 0) {
        tzset();
        ESP_LOGI(TAG, "timezone applied by offset: tz=%s offset=%ld", tz_buf, (long)offset_sec);
    } else {
        ESP_LOGW(TAG, "timezone apply by offset failed: offset=%ld", (long)offset_sec);
    }
}

static TickType_t weather_retry_ticks(void)
{
    gw_project_settings_t cfg = {0};
    if (gw_project_settings_get(&cfg) == ESP_OK) {
        return ms_to_ticks_safe(cfg.weather_retry_interval_ms);
    }
    return ms_to_ticks_safe(10 * 1000);
}

static TickType_t weather_success_ticks(void)
{
    gw_project_settings_t cfg = {0};
    if (gw_project_settings_get(&cfg) == ESP_OK) {
        return ms_to_ticks_safe(cfg.weather_success_interval_ms);
    }
    return ms_to_ticks_safe(60 * 60 * 1000);
}

static void apply_timezone_from_settings_or_geo(const s3_geoip_result_t *geo)
{
    gw_project_settings_t cfg = {0};
    if (gw_project_settings_get(&cfg) != ESP_OK) {
        if (geo) {
            if (geo->timezone[0]) {
                apply_timezone_if_present(geo->timezone);
            } else {
                apply_timezone_offset_if_present(geo->utc_offset_sec);
            }
        }
        return;
    }

    if (cfg.timezone_auto) {
        if (geo) {
            if (geo->timezone[0]) {
                apply_timezone_if_present(geo->timezone);
            } else {
                apply_timezone_offset_if_present(geo->utc_offset_sec);
            }
        }
        return;
    }

    const int32_t offset_sec = (int32_t)cfg.timezone_offset_min * 60;
    apply_timezone_offset_if_present(offset_sec);
}

static void timezone_label_for_state(char *out, size_t out_size, const s3_geoip_result_t *geo)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    gw_project_settings_t cfg = {0};
    if (gw_project_settings_get(&cfg) == ESP_OK && !cfg.timezone_auto) {
        const int off = (int)cfg.timezone_offset_min;
        const int abs_off = (off >= 0) ? off : -off;
        const int hh = abs_off / 60;
        const int mm = abs_off % 60;
        (void)snprintf(out, out_size, "UTC%c%02d:%02d", off >= 0 ? '+' : '-', hh, mm);
        return;
    }

    if (geo && geo->timezone[0]) {
        strlcpy(out, geo->timezone, out_size);
        return;
    }
    strlcpy(out, "UTC", out_size);
}

static void get_geo_snapshot(s3_geoip_result_t *out_geo)
{
    if (!out_geo) {
        return;
    }
    memset(out_geo, 0, sizeof(*out_geo));
    portENTER_CRITICAL(&s_lock);
    out_geo->latitude = s_geo_lat;
    out_geo->longitude = s_geo_lon;
    strlcpy(out_geo->timezone, s_geo_timezone, sizeof(out_geo->timezone));
    out_geo->utc_offset_sec = s_geo_offset_sec;
    out_geo->valid = s_geo_ready;
    portEXIT_CRITICAL(&s_lock);
}

static void apply_timezone_now_and_publish(const char *reason)
{
    s3_geoip_result_t geo = {0};
    get_geo_snapshot(&geo);

    apply_timezone_from_settings_or_geo(&geo);

    char tz_name[48] = {0};
    timezone_label_for_state(tz_name, sizeof(tz_name), &geo);
    persist_timezone_to_state_store(tz_name);

    char msg[96] = {0};
    (void)snprintf(msg, sizeof(msg), "tz=%s reason=%s", tz_name, reason ? reason : "unknown");
}

static void settings_event_listener(const gw_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event) {
        return;
    }
    if (strcmp(event->type, GW_EVT_SETTINGS_CHANGED) != 0) {
        return;
    }
    apply_timezone_now_and_publish(GW_EVT_SETTINGS_CHANGED);
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
    strlcpy(uid.uid, kWeatherUid, sizeof(uid.uid));
    const uint64_t ts_ms = now_ts_ms();
    (void)gw_state_store_set_f32(&uid, kWeatherEndpoint, "weather_lat", (float)lat, ts_ms);
    (void)gw_state_store_set_f32(&uid, kWeatherEndpoint, "weather_lon", (float)lon, ts_ms);
    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_lat",
                            kWeatherEndpoint, "weather_lat", 0, 0,
                            GW_EVENT_VALUE_F64, false, 0, lat, NULL, NULL, 0);
    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_lon",
                            kWeatherEndpoint, "weather_lon", 0, 0,
                            GW_EVENT_VALUE_F64, false, 0, lon, NULL, NULL, 0);
}

static void persist_location_to_state_store(const char *location)
{
    if (!location || !location[0]) {
        return;
    }

    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, kWeatherUid, sizeof(uid.uid));
    const uint64_t ts_ms = now_ts_ms();
    (void)gw_state_store_set_text(&uid, kWeatherEndpoint, "weather_location", location, ts_ms);
    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_location",
                            kWeatherEndpoint, "weather_location", 0, 0,
                            GW_EVENT_VALUE_TEXT, false, 0, 0.0, location, NULL, 0);
}

static void persist_weather_status_to_state_store(const char *status)
{
    if (!status) {
        return;
    }

    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, kWeatherUid, sizeof(uid.uid));
    const uint64_t ts_ms = now_ts_ms();
    (void)gw_state_store_set_text(&uid, kWeatherEndpoint, "weather_status", status, ts_ms);
    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_status",
                            kWeatherEndpoint, "weather_status", 0, 0,
                            GW_EVENT_VALUE_TEXT, false, 0, 0.0, status, NULL, 0);
}

static void persist_timezone_to_state_store(const char *tz_name)
{
    if (!tz_name || !tz_name[0]) {
        return;
    }
    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, kWeatherUid, sizeof(uid.uid));
    const uint64_t ts_ms = now_ts_ms();
    (void)gw_state_store_set_text(&uid, kWeatherEndpoint, "weather_tz", tz_name, ts_ms);
    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_tz",
                            kWeatherEndpoint, "weather_tz", 0, 0,
                            GW_EVENT_VALUE_TEXT, false, 0, 0.0, tz_name, NULL, 0);
}

static esp_err_t ensure_geo_location(void)
{
    const uint64_t now_ms = now_ts_ms();
    const bool refresh_due = (s_last_geo_refresh_ms == 0) ||
                             (now_ms > 0 && (now_ms - s_last_geo_refresh_ms) >= kGeoRefreshPeriodMs);
    if (s_geo_ready && !refresh_due) {
        return ESP_OK;
    }

    s3_geoip_result_t geo = {0};
    char err[128] = {0};
    esp_err_t geo_err = s3_geoip_http_fetch_once(8000, &geo, err, sizeof(err));
    if (geo_err != ESP_OK || !geo.valid) {
        ESP_LOGW(TAG, "geoip failed: err=%s detail=%s", esp_err_to_name(geo_err), err[0] ? err : "-");
        if (geo_err == ESP_ERR_INVALID_RESPONSE) {
            set_location_text("Location service error");
            persist_location_to_state_store("Location service error");
        } else {
            set_location_text("Location unavailable");
            persist_location_to_state_store("Location unavailable");
        }
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_lock);
    s_geo_lat = geo.latitude;
    s_geo_lon = geo.longitude;
    strlcpy(s_geo_timezone, geo.timezone, sizeof(s_geo_timezone));
    s_geo_offset_sec = geo.utc_offset_sec;
    s_geo_ready = true;
    s_last_geo_refresh_ms = now_ms;
    portEXIT_CRITICAL(&s_lock);

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
    persist_location_to_state_store(loc);
    persist_weather_status_to_state_store("location_ready");
    apply_timezone_from_settings_or_geo(&geo);
    char tz_name[48] = {0};
    timezone_label_for_state(tz_name, sizeof(tz_name), &geo);
    persist_timezone_to_state_store(tz_name);
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
    strlcpy(uid.uid, kWeatherUid, sizeof(uid.uid));
    const uint64_t ts_ms = now_ts_ms();

    (void)gw_state_store_set_f32(&uid, kWeatherEndpoint, "weather_temp_c", res->temperature_c, ts_ms);
    (void)gw_state_store_set_f32(&uid, kWeatherEndpoint, "weather_humidity_pct", res->humidity_pct, ts_ms);
    (void)gw_state_store_set_f32(&uid, kWeatherEndpoint, "weather_wind_kmh", res->wind_speed_kmh, ts_ms);
    (void)gw_state_store_set_u32(&uid, kWeatherEndpoint, "weather_code", (uint32_t)res->weather_code, ts_ms);
    (void)gw_state_store_set_u64(&uid, kWeatherEndpoint, "weather_updated_ms", ts_ms, ts_ms);

    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_temp_c",
                            kWeatherEndpoint, "weather_temp_c", 0, 0,
                            GW_EVENT_VALUE_F64, false, 0, res->temperature_c, NULL, NULL, 0);
    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_humidity_pct",
                            kWeatherEndpoint, "weather_humidity_pct", 0, 0,
                            GW_EVENT_VALUE_F64, false, 0, res->humidity_pct, NULL, NULL, 0);
    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_wind_kmh",
                            kWeatherEndpoint, "weather_wind_kmh", 0, 0,
                            GW_EVENT_VALUE_F64, false, 0, res->wind_speed_kmh, NULL, NULL, 0);
    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_code",
                            kWeatherEndpoint, "weather_code", 0, 0,
                            GW_EVENT_VALUE_I64, false, (int64_t)res->weather_code, 0.0, NULL, NULL, 0);
    gw_event_bus_publish_zb(GW_EVT_DEVICE_STATE, "weather", kWeatherUid, 0, "weather_updated_ms",
                            kWeatherEndpoint, "weather_updated_ms", 0, 0,
                            GW_EVENT_VALUE_I64, false, (int64_t)ts_ms, 0.0, NULL, NULL, 0);
}

static void weather_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (ensure_geo_location() != ESP_OK) {
            vTaskDelay(weather_retry_ticks());
            continue;
        }

        s3_geoip_result_t geo_for_tz = {0};
        get_geo_snapshot(&geo_for_tz);
        apply_timezone_from_settings_or_geo(&geo_for_tz);

        s3_weather_result_t res = {0};
        char err[128] = {0};
        esp_err_t fetch_err = s3_weather_http_fetch_once(s_geo_lat, s_geo_lon, 8000, &res, err, sizeof(err));
        if (fetch_err != ESP_OK || !res.valid) {
            ESP_LOGW(TAG, "weather fetch failed: err=%s detail=%s",
                     esp_err_to_name(fetch_err), err[0] ? err : "-");
            if (fetch_err == ESP_ERR_INVALID_RESPONSE) {
                persist_weather_status_to_state_store("weather_service_error");
            } else {
                persist_weather_status_to_state_store("weather_unavailable");
            }
            vTaskDelay(weather_retry_ticks());
            continue;
        }

        persist_weather_to_state_store(&res);
        persist_weather_status_to_state_store("weather_ready");
        ESP_LOGI(TAG,
                 "weather updated: t=%.1fC h=%.1f%% wind=%.1fkm/h code=%d obs=%s",
                 (double)res.temperature_c,
                 (double)res.humidity_pct,
                 (double)res.wind_speed_kmh,
                 res.weather_code,
                 res.observed_time);
        vTaskDelay(weather_success_ticks());
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
    persist_location_to_state_store("Locating...");
    persist_weather_status_to_state_store("starting");
    if (!s_listener_registered) {
        if (gw_event_bus_add_listener(settings_event_listener, NULL) == ESP_OK) {
            s_listener_registered = true;
        } else {
            ESP_LOGW(TAG, "failed to register settings listener");
        }
    }
    return ESP_OK;
}
