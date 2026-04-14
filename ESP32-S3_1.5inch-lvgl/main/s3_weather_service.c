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
#include "gw_core/net_time.h"
#include "gw_core/gw_proto_bus.h"
#include "gw_model/gw_model_settings.h"
#include "gw_model/gw_model_device_meta.h"
#include "gw_model/gw_model_state.h"
#include "gw_model/gw_model_topology.h"

static const char *TAG = "s3_weather_svc";

static const char *kWeatherUid = "0xweather000000001";
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

static void persist_timezone_to_model(const char *tz_name);
static uint64_t now_ts_ms(void);
static esp_err_t load_settings(gw_proto_settings_v1_t *out);
static void settings_proto_listener(gw_proto_bus_channel_t channel,
                                    const gw_proto_hdr_t *hdr,
                                    const void *payload,
                                    void *user_ctx);

static void ensure_weather_model(void)
{
    gw_device_uid_t uid = {0};
    strlcpy(uid.uid, kWeatherUid, sizeof(uid.uid));

    gw_proto_device_v1_t dev = {0};
    dev.device_uid = uid;
    dev.short_addr = 0;
    dev.version = 1;
    dev.last_seen_ms = now_ts_ms();
    (void)gw_model_set_device_name(&uid, "Weather", NULL);
    (void)gw_model_upsert_device(&dev, NULL, NULL);

    gw_proto_endpoint_v1_t ep = {0};
    ep.uid = uid;
    ep.short_addr = 0;
    ep.endpoint = kWeatherEndpoint;
    ep.version = 1;
    ep.profile_id = 0x0104;
    ep.device_id = 0x0302;
    ep.in_cluster_count = 2;
    ep.out_cluster_count = 0;
    ep.in_clusters[0] = 0x0402;
    ep.in_clusters[1] = 0x0405;
    (void)gw_model_upsert_endpoint(&ep, NULL, NULL);
}

static void weather_state_upsert_text(const char *key, const char *value, uint64_t ts_ms)
{
    gw_proto_state_item_v1_t item = {0};
    strlcpy(item.uid.uid, kWeatherUid, sizeof(item.uid.uid));
    item.endpoint = kWeatherEndpoint;
    item.value_type = GW_STATE_VALUE_TEXT;
    strlcpy(item.key, key, sizeof(item.key));
    strlcpy(item.value_text, value ? value : "", sizeof(item.value_text));
    item.ts_ms = ts_ms;
    (void)gw_model_upsert_state(&item, NULL, NULL);
}

static void weather_state_upsert_f32(const char *key, float value, uint64_t ts_ms)
{
    gw_proto_state_item_v1_t item = {0};
    strlcpy(item.uid.uid, kWeatherUid, sizeof(item.uid.uid));
    item.endpoint = kWeatherEndpoint;
    item.value_type = GW_STATE_VALUE_F32;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_f32 = value;
    item.ts_ms = ts_ms;
    (void)gw_model_upsert_state(&item, NULL, NULL);
}

static void weather_state_upsert_u32(const char *key, uint32_t value, uint64_t ts_ms)
{
    gw_proto_state_item_v1_t item = {0};
    strlcpy(item.uid.uid, kWeatherUid, sizeof(item.uid.uid));
    item.endpoint = kWeatherEndpoint;
    item.value_type = GW_STATE_VALUE_U32;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_u32 = value;
    item.ts_ms = ts_ms;
    (void)gw_model_upsert_state(&item, NULL, NULL);
}

static void weather_state_upsert_u64(const char *key, uint64_t value, uint64_t ts_ms)
{
    gw_proto_state_item_v1_t item = {0};
    strlcpy(item.uid.uid, kWeatherUid, sizeof(item.uid.uid));
    item.endpoint = kWeatherEndpoint;
    item.value_type = GW_STATE_VALUE_U64;
    strlcpy(item.key, key, sizeof(item.key));
    item.value_u64 = value;
    item.ts_ms = ts_ms;
    (void)gw_model_upsert_state(&item, NULL, NULL);
}

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
    gw_proto_settings_v1_t cfg = {0};
    if (load_settings(&cfg) == ESP_OK) {
        return ms_to_ticks_safe(cfg.weather_retry_interval_ms);
    }
    ESP_LOGI(TAG, "using default retry interval: 10sec");
    return ms_to_ticks_safe(10 * 1000);
}

static TickType_t weather_success_ticks(void)
{
    gw_proto_settings_v1_t cfg = {0};
    if (load_settings(&cfg) == ESP_OK) {
        ESP_LOGI(TAG, "weather interval: success=%usec retry=%usec",
                 (unsigned)(cfg.weather_success_interval_ms / 1000),
                 (unsigned)(cfg.weather_retry_interval_ms / 1000));
        return ms_to_ticks_safe(cfg.weather_success_interval_ms);
    }
    return ms_to_ticks_safe(60 * 60 * 1000);
}

static void weather_wait(TickType_t ticks)
{
    (void)ulTaskNotifyTake(pdTRUE, ticks);
}

static void weather_wake(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void apply_timezone_from_settings_or_geo(const s3_geoip_result_t *geo)
{
    gw_proto_settings_v1_t cfg = {0};
    if (load_settings(&cfg) != ESP_OK) {
        if (geo) {
            if (geo->timezone[0]) {
                apply_timezone_if_present(geo->timezone);
            } else {
                apply_timezone_offset_if_present(geo->utc_offset_sec);
            }
        }
        return;
    }

    if (cfg.timezone_auto != 0) {
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

    gw_proto_settings_v1_t cfg = {0};
    if (load_settings(&cfg) == ESP_OK && cfg.timezone_auto == 0) {
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
    persist_timezone_to_model(tz_name);

    (void)reason;
}

static void apply_weather_settings_change(const gw_proto_settings_v1_t *settings)
{
    if (!settings) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_last_geo_refresh_ms = 0;
    if (!settings->weather_location_auto && settings->weather_city[0]) {
        s_geo_lat = settings->weather_lat;
        s_geo_lon = settings->weather_lon;
        s_geo_ready = true;
    } else {
        s_geo_ready = false;
    }
    portEXIT_CRITICAL(&s_lock);

    weather_wake();
}

static esp_err_t load_settings(gw_proto_settings_v1_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    return gw_model_get_settings(out);
}

static void settings_proto_listener(gw_proto_bus_channel_t channel,
                                    const gw_proto_hdr_t *hdr,
                                    const void *payload,
                                    void *user_ctx)
{
    (void)channel;
    (void)user_ctx;
    if (!hdr || !payload || hdr->type != GW_PROTO_MSG_SETTINGS || hdr->len < sizeof(gw_proto_settings_v1_t)) {
        return;
    }
    apply_timezone_now_and_publish("settings.changed");
    apply_weather_settings_change((const gw_proto_settings_v1_t *)payload);
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

static void persist_geo_to_model(double lat, double lon)
{
    const uint64_t ts_ms = now_ts_ms();
    weather_state_upsert_f32("weather_lat", (float)lat, ts_ms);
    weather_state_upsert_f32("weather_lon", (float)lon, ts_ms);
}

static void persist_location_to_model(const char *location)
{
    if (!location || !location[0]) {
        return;
    }
    const uint64_t ts_ms = now_ts_ms();
    weather_state_upsert_text("weather_location", location, ts_ms);
}

static void persist_weather_status_to_model(const char *status)
{
    if (!status) {
        return;
    }
    const uint64_t ts_ms = now_ts_ms();
    weather_state_upsert_text("weather_status", status, ts_ms);
}

static void persist_timezone_to_model(const char *tz_name)
{
    if (!tz_name || !tz_name[0]) {
        return;
    }
    const uint64_t ts_ms = now_ts_ms();
    weather_state_upsert_text("weather_tz", tz_name, ts_ms);
}

static esp_err_t ensure_geo_location(void)
{
    gw_proto_settings_v1_t settings = {0};
    (void)load_settings(&settings);

    if (!settings.weather_location_auto && settings.weather_city[0]) {
        portENTER_CRITICAL(&s_lock);
        s_geo_lat = settings.weather_lat;
        s_geo_lon = settings.weather_lon;
        s_geo_ready = true;
        s_last_geo_refresh_ms = now_ts_ms();
        portEXIT_CRITICAL(&s_lock);

        set_location_text(settings.weather_city);
        persist_location_to_model(settings.weather_city);
        persist_weather_status_to_model("location_ready");
        persist_geo_to_model(s_geo_lat, s_geo_lon);
        ESP_LOGI(TAG, "using manual location: %s (lat=%.6f lon=%.6f)",
                 settings.weather_city, s_geo_lat, s_geo_lon);
        return ESP_OK;
    }

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
            persist_location_to_model("Location service error");
        } else {
            set_location_text("Location unavailable");
            persist_location_to_model("Location unavailable");
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
    persist_location_to_model(loc);
    persist_weather_status_to_model("location_ready");
    apply_timezone_from_settings_or_geo(&geo);
    char tz_name[48] = {0};
    timezone_label_for_state(tz_name, sizeof(tz_name), &geo);
    persist_timezone_to_model(tz_name);
    persist_geo_to_model(s_geo_lat, s_geo_lon);
    ESP_LOGI(TAG, "geoip resolved: %s (lat=%.6f lon=%.6f)", loc, s_geo_lat, s_geo_lon);
    return ESP_OK;
}

static void persist_weather_to_model(const s3_weather_result_t *res)
{
    if (!res || !res->valid) {
        return;
    }

    const uint64_t ts_ms = now_ts_ms();

    weather_state_upsert_f32("weather_temp_c", res->temperature_c, ts_ms);
    weather_state_upsert_f32("weather_humidity_pct", res->humidity_pct, ts_ms);
    weather_state_upsert_f32("weather_wind_kmh", res->wind_speed_kmh, ts_ms);
    weather_state_upsert_u32("weather_code", (uint32_t)res->weather_code, ts_ms);
    weather_state_upsert_u64("weather_updated_ms", ts_ms, ts_ms);

}

static void weather_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (ensure_geo_location() != ESP_OK) {
            weather_wait(weather_retry_ticks());
            continue;
        }

        s3_geoip_result_t geo_for_tz = {0};
        get_geo_snapshot(&geo_for_tz);
        apply_timezone_from_settings_or_geo(&geo_for_tz);

        ESP_LOGI(TAG, "fetching weather: lat=%.6f lon=%.6f", s_geo_lat, s_geo_lon);
        s3_weather_result_t res = {0};
        char err[128] = {0};
        esp_err_t fetch_err = s3_weather_http_fetch_once(s_geo_lat, s_geo_lon, 8000, &res, err, sizeof(err));
        if (fetch_err != ESP_OK || !res.valid) {
            ESP_LOGW(TAG, "weather fetch failed: err=%s detail=%s",
                     esp_err_to_name(fetch_err), err[0] ? err : "-");
            if (fetch_err == ESP_ERR_INVALID_RESPONSE) {
                persist_weather_status_to_model("weather_service_error");
            } else {
                persist_weather_status_to_model("weather_unavailable");
            }
            weather_wait(weather_retry_ticks());
            continue;
        }

        persist_weather_to_model(&res);
        persist_weather_status_to_model("weather_ready");
        char location[64] = {0};
        s3_weather_service_get_location(location, sizeof(location));
        ESP_LOGI(TAG,
                 "weather updated: location=%s lat=%.6f lon=%.6f t=%.1fC h=%.1f%% wind=%.1fkm/h code=%d obs=%s",
                 location[0] ? location : "-",
                 s_geo_lat,
                 s_geo_lon,
                 (double)res.temperature_c,
                 (double)res.humidity_pct,
                 (double)res.wind_speed_kmh,
                 res.weather_code,
                 res.observed_time);
        weather_wait(weather_success_ticks());
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
    ensure_weather_model();
    persist_location_to_model("Locating...");
    persist_weather_status_to_model("starting");
    if (!s_listener_registered) {
        if (gw_proto_bus_add_listener(settings_proto_listener, GW_PROTO_BUS_CHANNEL_MODEL, NULL) == ESP_OK) {
            s_listener_registered = true;
        } else {
            ESP_LOGW(TAG, "failed to register settings listener");
        }
    }
    return ESP_OK;
}
