#include "gw_core/weather.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "gw_core/event_bus.h"
#include "gw_core/net_fetch.h"

static const char *TAG = "gw_weather";

static const uint32_t kDefaultRefreshIntervalMs = 60u * 60u * 1000u;
static const uint32_t kDefaultRequestTimeoutMs = 8000u;
static const uint32_t kRetryIntervalMs = 5000u;
static const size_t kWeatherTaskStackBytes = 8192;
static const char *kDefaultWeatherBaseUrl = "http://api.open-meteo.com/v1/forecast";

static TaskHandle_t s_task = NULL;
static bool s_initialized = false;
static bool s_bootstrap_done = false;
static gw_weather_cfg_t s_cfg = {0};
static gw_weather_snapshot_t s_snapshot = {0};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint64_t mono_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t fetch_snapshot(gw_weather_snapshot_t *out_snapshot)
{
    if (!out_snapshot) {
        return ESP_ERR_INVALID_ARG;
    }

    char *url = (char *)heap_caps_calloc(1, 512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!url) {
        url = (char *)heap_caps_calloc(1, 512, MALLOC_CAP_8BIT);
    }
    if (!url) {
        return ESP_ERR_NO_MEM;
    }

    const int n = snprintf(
        url,
        512,
        "%s?latitude=%.6f&longitude=%.6f&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto",
        kDefaultWeatherBaseUrl,
        s_cfg.latitude,
        s_cfg.longitude
    );
    if (n <= 0 || n >= 512) {
        free(url);
        return ESP_ERR_INVALID_SIZE;
    }

    gw_net_fetch_cfg_t fetch_cfg = {
        .timeout_ms = (int)s_cfg.request_timeout_ms,
        .max_body_bytes = 2048,
    };

    char *body = (char *)heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = (char *)heap_caps_calloc(1, 2048, MALLOC_CAP_8BIT);
    }
    if (!body) {
        free(url);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "weather fetch start lat=%.6f lon=%.6f", s_cfg.latitude, s_cfg.longitude);

    int http_status = 0;
    esp_err_t err = gw_net_fetch_get_text(url, &fetch_cfg, body, 2048, &http_status);
    free(url);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "weather fetch transport failed: err=%s http=%d", esp_err_to_name(err), http_status);
        free(body);
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *temperature = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "temperature_2m") : NULL;
    const cJSON *humidity = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "relative_humidity_2m") : NULL;
    const cJSON *wind = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "wind_speed_10m") : NULL;
    const cJSON *code = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "weather_code") : NULL;
    const cJSON *obs_time = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "time") : NULL;

    if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(humidity) || !cJSON_IsNumber(wind) || !cJSON_IsNumber(code)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    gw_weather_snapshot_t snap = {0};
    snap.valid = true;
    snap.temperature_c = (float)temperature->valuedouble;
    snap.humidity_pct = (float)humidity->valuedouble;
    snap.wind_speed_kmh = (float)wind->valuedouble;
    snap.weather_code = code->valueint;
    snap.updated_mono_ms = mono_now_ms();
    if (cJSON_IsString(obs_time) && obs_time->valuestring) {
        strlcpy(snap.observed_time, obs_time->valuestring, sizeof(snap.observed_time));
    } else {
        snap.observed_time[0] = '\0';
    }

    *out_snapshot = snap;
    ESP_LOGI(TAG, "weather fetch ok: http=%d observed=%s", http_status, snap.observed_time);
    cJSON_Delete(root);
    return ESP_OK;
}

static void weather_task(void *arg)
{
    (void)arg;

    const TickType_t interval_ticks = pdMS_TO_TICKS(s_cfg.refresh_interval_ms);
    const TickType_t retry_ticks = pdMS_TO_TICKS(kRetryIntervalMs);
    TickType_t wait_ticks = 0; // Immediate first fetch on startup.
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);

        gw_weather_snapshot_t snap = {0};
        esp_err_t err = fetch_snapshot(&snap);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "weather update failed: %s", esp_err_to_name(err));
            gw_event_bus_publish("weather.update_failed", "weather", "", 0, esp_err_to_name(err));
            wait_ticks = retry_ticks;
            continue;
        }

        portENTER_CRITICAL(&s_lock);
        s_snapshot = snap;
        s_bootstrap_done = true;
        portEXIT_CRITICAL(&s_lock);

        ESP_LOGI(TAG, "weather updated: t=%.1fC h=%.1f%% wind=%.1fkm/h code=%d",
                 (double)snap.temperature_c,
                 (double)snap.humidity_pct,
                 (double)snap.wind_speed_kmh,
                 snap.weather_code);
        char msg[96];
        (void)snprintf(msg, sizeof(msg), "t=%.1f h=%.1f wind=%.1f code=%d",
                       (double)snap.temperature_c,
                       (double)snap.humidity_pct,
                       (double)snap.wind_speed_kmh,
                       snap.weather_code);
        gw_event_bus_publish("weather.updated", "weather", "", 0, msg);
        wait_ticks = interval_ticks;
    }
}

esp_err_t gw_weather_init(const gw_weather_cfg_t *cfg)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    if (cfg) {
        s_cfg = *cfg;
    }

    if (s_cfg.refresh_interval_ms == 0) {
        s_cfg.refresh_interval_ms = kDefaultRefreshIntervalMs;
    }
    if (s_cfg.request_timeout_ms == 0) {
        s_cfg.request_timeout_ms = kDefaultRequestTimeoutMs;
    }
    // Always do immediate first fetch in weather_task; cfg.refresh_on_init is ignored.
    if (s_cfg.latitude < -90.0 || s_cfg.latitude > 90.0 || s_cfg.longitude < -180.0 || s_cfg.longitude > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }

    BaseType_t ok = xTaskCreateWithCaps(
        weather_task,
        "gw_weather",
        kWeatherTaskStackBytes,
        NULL,
        2,
        &s_task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ok != pdTRUE) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    s_bootstrap_done = false;
    ESP_LOGI(TAG,
             "initialized lat=%.6f lon=%.6f interval_ms=%u",
             s_cfg.latitude,
             s_cfg.longitude,
             (unsigned)s_cfg.refresh_interval_ms);
    return ESP_OK;
}

esp_err_t gw_weather_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    TaskHandle_t task = s_task;
    s_task = NULL;
    if (task) {
        vTaskDelete(task);
    }

    portENTER_CRITICAL(&s_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_bootstrap_done = false;
    portEXIT_CRITICAL(&s_lock);

    s_initialized = false;
    return ESP_OK;
}

bool gw_weather_is_ready(void)
{
    bool valid = false;
    portENTER_CRITICAL(&s_lock);
    valid = s_snapshot.valid;
    portEXIT_CRITICAL(&s_lock);
    return valid;
}

bool gw_weather_bootstrap_done(void)
{
    bool done = false;
    portENTER_CRITICAL(&s_lock);
    done = s_bootstrap_done;
    portEXIT_CRITICAL(&s_lock);
    return done;
}

esp_err_t gw_weather_request_refresh(void)
{
    if (!s_initialized || !s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ok = xTaskNotifyGive(s_task);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_weather_get_snapshot(gw_weather_snapshot_t *out_snapshot)
{
    if (!out_snapshot) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    *out_snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
    return out_snapshot->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}
