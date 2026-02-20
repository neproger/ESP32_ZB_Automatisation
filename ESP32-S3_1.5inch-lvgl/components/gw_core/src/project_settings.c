#include "gw_core/project_settings.h"

#include <string.h>

#include "esp_log.h"
#include "gw_core/storage.h"

static const char *TAG = "gw_settings";

static const uint32_t SETTINGS_MAGIC = 0x53545447; // STTG
static const uint16_t SETTINGS_VERSION = 1;

static const uint32_t kDefaultScreensaverTimeoutMs = 4000;
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

static gw_storage_t s_settings_storage;
static bool s_inited = false;

static const gw_storage_desc_t s_settings_desc = {
    .key = "proj_settings",
    .item_size = sizeof(gw_project_settings_t),
    .max_items = 1,
    .magic = SETTINGS_MAGIC,
    .version = SETTINGS_VERSION,
    .namespace = "settings",
};

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

static esp_err_t persist_current(void)
{
    return gw_storage_save(&s_settings_storage);
}

esp_err_t gw_project_settings_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t err = gw_storage_init(&s_settings_storage, &s_settings_desc, GW_STORAGE_NVS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings storage init failed: %s", esp_err_to_name(err));
        return err;
    }

    bool need_persist = false;
    bool init_defaults = false;
    bool repaired_invalid = false;
    portENTER_CRITICAL(&s_settings_storage.lock);
    if (s_settings_storage.count == 0) {
        gw_project_settings_t defaults = {0};
        gw_project_settings_get_defaults(&defaults);
        ((gw_project_settings_t *)s_settings_storage.data)[0] = defaults;
        s_settings_storage.count = 1;
        need_persist = true;
        init_defaults = true;
    } else if (s_settings_storage.count > 1) {
        gw_project_settings_t keep = ((gw_project_settings_t *)s_settings_storage.data)[0];
        if (!gw_project_settings_validate(&keep)) {
            gw_project_settings_get_defaults(&keep);
        }
        ((gw_project_settings_t *)s_settings_storage.data)[0] = keep;
        s_settings_storage.count = 1;
        need_persist = true;
    } else {
        gw_project_settings_t cur = ((gw_project_settings_t *)s_settings_storage.data)[0];
        if (!gw_project_settings_validate(&cur)) {
            gw_project_settings_get_defaults(&cur);
            ((gw_project_settings_t *)s_settings_storage.data)[0] = cur;
            need_persist = true;
            repaired_invalid = true;
        }
    }
    portEXIT_CRITICAL(&s_settings_storage.lock);

    if (need_persist) {
        esp_err_t save_err = persist_current();
        if (save_err != ESP_OK) {
            return save_err;
        }
    }
    if (init_defaults) {
        ESP_LOGI(TAG, "project settings initialized with defaults");
    } else if (repaired_invalid) {
        ESP_LOGW(TAG, "invalid persisted settings replaced with defaults");
    }

    s_inited = true;
    return ESP_OK;
}

esp_err_t gw_project_settings_get(gw_project_settings_t *out)
{
    if (!s_inited || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_settings_storage.lock);
    *out = ((gw_project_settings_t *)s_settings_storage.data)[0];
    portEXIT_CRITICAL(&s_settings_storage.lock);
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

    portENTER_CRITICAL(&s_settings_storage.lock);
    ((gw_project_settings_t *)s_settings_storage.data)[0] = *in;
    s_settings_storage.count = 1;
    portEXIT_CRITICAL(&s_settings_storage.lock);

    return persist_current();
}
