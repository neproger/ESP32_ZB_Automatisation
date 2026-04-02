#include "gw_core/project_settings.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gw_core/gw_proto_bus.h"
#include "gw_model/gw_model_settings.h"
#include "gw_proto/gw_proto_map.h"

static const char *TAG = "gw_settings";

static const uint32_t kDefaultScreensaverTimeoutMs = 10000;
static const uint32_t kDefaultWeatherSuccessIntervalMs = 60 * 60 * 1000;
static const uint32_t kDefaultWeatherRetryIntervalMs = 10 * 1000;
static const bool kDefaultTimezoneAuto = true;
static const int16_t kDefaultTimezoneOffsetMin = 0;

static const uint32_t kMinScreensaverTimeoutMs = 1000;
static const uint32_t kMaxScreensaverTimeoutMs = 600 * 1000;
static const uint32_t kMinWeatherSuccessIntervalMs = 60 * 1000;
static const uint32_t kMaxWeatherSuccessIntervalMs = 24 * 60 * 60 * 1000;
static const uint32_t kMinWeatherRetryIntervalMs = 3000;
static const uint32_t kMaxWeatherRetryIntervalMs = 10 * 60 * 1000;
static const int16_t kMinTimezoneOffsetMin = -12 * 60;
static const int16_t kMaxTimezoneOffsetMin = 14 * 60;

static bool s_inited = false;

#define GW_SETTINGS_LISTENER_CAP 4
typedef struct {
    gw_project_settings_listener_t cb;
    void *user_ctx;
} gw_settings_listener_slot_t;

static gw_settings_listener_slot_t s_listeners[GW_SETTINGS_LISTENER_CAP];
static portMUX_TYPE s_listener_lock = portMUX_INITIALIZER_UNLOCKED;

static void proto_to_project(const gw_proto_settings_v1_t *src, gw_project_settings_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    dst->screensaver_timeout_ms = src->screensaver_timeout_ms;
    dst->weather_success_interval_ms = src->weather_success_interval_ms;
    dst->weather_retry_interval_ms = src->weather_retry_interval_ms;
    dst->timezone_auto = src->timezone_auto != 0;
    dst->timezone_offset_min = src->timezone_offset_min;
}

static void project_to_proto(const gw_project_settings_t *src, gw_proto_settings_v1_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    dst->screensaver_timeout_ms = src->screensaver_timeout_ms;
    dst->weather_success_interval_ms = src->weather_success_interval_ms;
    dst->weather_retry_interval_ms = src->weather_retry_interval_ms;
    dst->timezone_auto = src->timezone_auto ? 1u : 0u;
    dst->timezone_offset_min = src->timezone_offset_min;
}

static void notify_settings_listeners(const gw_project_settings_t *settings)
{
    if (settings) {
        gw_proto_settings_v1_t msg = {0};
        gw_proto_hdr_t hdr = {0};
        gw_proto_fill_settings(&msg, settings);
        gw_proto_fill_hdr(&hdr, GW_PROTO_MSG_SETTINGS, sizeof(msg), 0);
        (void)gw_proto_bus_publish(GW_PROTO_BUS_CHANNEL_MODEL, &hdr, &msg);
    }

    gw_settings_listener_slot_t listeners[GW_SETTINGS_LISTENER_CAP];
    size_t listener_count = 0;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_SETTINGS_LISTENER_CAP; i++) {
        if (s_listeners[i].cb) {
            listeners[listener_count++] = s_listeners[i];
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);

    for (size_t i = 0; i < listener_count; i++) {
        listeners[i].cb(settings, listeners[i].user_ctx);
    }
}

void gw_project_settings_get_defaults(gw_project_settings_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->screensaver_timeout_ms = kDefaultScreensaverTimeoutMs;
    out->weather_success_interval_ms = kDefaultWeatherSuccessIntervalMs;
    out->weather_retry_interval_ms = kDefaultWeatherRetryIntervalMs;
    out->timezone_auto = kDefaultTimezoneAuto;
    out->timezone_offset_min = kDefaultTimezoneOffsetMin;
}

bool gw_project_settings_validate(const gw_project_settings_t *in)
{
    if (!in) {
        return false;
    }
    if (in->screensaver_timeout_ms < kMinScreensaverTimeoutMs ||
        in->screensaver_timeout_ms > kMaxScreensaverTimeoutMs) {
        return false;
    }
    if (in->weather_success_interval_ms < kMinWeatherSuccessIntervalMs ||
        in->weather_success_interval_ms > kMaxWeatherSuccessIntervalMs) {
        return false;
    }
    if (in->weather_retry_interval_ms < kMinWeatherRetryIntervalMs ||
        in->weather_retry_interval_ms > kMaxWeatherRetryIntervalMs) {
        return false;
    }
    if (in->timezone_offset_min < kMinTimezoneOffsetMin ||
        in->timezone_offset_min > kMaxTimezoneOffsetMin) {
        return false;
    }
    return true;
}

esp_err_t gw_project_settings_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    gw_proto_settings_v1_t stored = {0};
    esp_err_t err = gw_model_get_settings(&stored);
    if (err == ESP_ERR_NOT_FOUND) {
        gw_project_settings_t defaults = {0};
        gw_proto_settings_v1_t proto_defaults = {0};
        gw_project_settings_get_defaults(&defaults);
        project_to_proto(&defaults, &proto_defaults);
        err = gw_model_set_settings(&proto_defaults, NULL, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "settings default init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_inited = true;
        ESP_LOGI(TAG, "project settings initialized with defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings load failed: %s", esp_err_to_name(err));
        return err;
    }

    gw_project_settings_t cur = {0};
    proto_to_project(&stored, &cur);
    if (!gw_project_settings_validate(&cur)) {
        gw_project_settings_get_defaults(&cur);
        gw_proto_settings_v1_t repaired = {0};
        project_to_proto(&cur, &repaired);
        err = gw_model_set_settings(&repaired, NULL, NULL);
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "invalid persisted settings replaced with defaults");
    }

    s_inited = true;
    return ESP_OK;
}

esp_err_t gw_project_settings_add_listener(gw_project_settings_listener_t cb, void *user_ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_SETTINGS_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < GW_SETTINGS_LISTENER_CAP; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i].cb = cb;
            s_listeners[i].user_ctx = user_ctx;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t gw_project_settings_remove_listener(gw_project_settings_listener_t cb, void *user_ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_listener_lock);
    for (size_t i = 0; i < GW_SETTINGS_LISTENER_CAP; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].user_ctx == user_ctx) {
            s_listeners[i].cb = NULL;
            s_listeners[i].user_ctx = NULL;
            portEXIT_CRITICAL(&s_listener_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_listener_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t gw_project_settings_get(gw_project_settings_t *out)
{
    if (!s_inited || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_settings_v1_t stored = {0};
    esp_err_t err = gw_model_get_settings(&stored);
    if (err != ESP_OK) {
        return err;
    }
    proto_to_project(&stored, out);
    return ESP_OK;
}

esp_err_t gw_project_settings_set(const gw_project_settings_t *in)
{
    if (!s_inited || !in) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gw_project_settings_validate(in)) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_settings_v1_t record = {0};
    project_to_proto(in, &record);
    esp_err_t err = gw_model_set_settings(&record, NULL, NULL);
    if (err == ESP_OK) {
        notify_settings_listeners(in);
    }
    return err;
}
