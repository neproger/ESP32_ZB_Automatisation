#include "gw_model/gw_model_settings.h"

#include <string.h>

#include "micro_db/micro_db_core.h"
#include "gw_model_notify.h"
#include "gw_model/gw_model_schema.h"
#include "gw_proto/gw_proto_validate.h"

static micro_db_table_t s_settings_table;
static const uint8_t k_settings_key = 1u;
static const uint32_t kDefaultScreensaverTimeoutMs = 10000;
static const uint32_t kDefaultWeatherSuccessIntervalMs = 60 * 60 * 1000;
static const uint32_t kDefaultWeatherRetryIntervalMs = 10 * 1000;
static const uint8_t kDefaultTimezoneAuto = 1u;
static const uint8_t kDefaultWeatherLocationAuto = 1u;
static const int16_t kDefaultTimezoneOffsetMin = 0;
static const uint32_t kMinScreensaverTimeoutMs = 1000;
static const uint32_t kMaxScreensaverTimeoutMs = 600 * 1000;
static const uint32_t kMinWeatherSuccessIntervalMs = 60 * 1000;
static const uint32_t kMaxWeatherSuccessIntervalMs = 24 * 60 * 60 * 1000;
static const uint32_t kMinWeatherRetryIntervalMs = 3000;
static const uint32_t kMaxWeatherRetryIntervalMs = 10 * 60 * 1000;
static const int16_t kMinTimezoneOffsetMin = -12 * 60;
static const int16_t kMaxTimezoneOffsetMin = 14 * 60;

esp_err_t gw_model_init_settings(void)
{
    esp_err_t err = micro_db_table_init(&s_settings_table, &GW_MODEL_SCHEMA_SETTINGS);
    if (err != ESP_OK) {
        return err;
    }

    gw_proto_settings_v1_t current = {0};
    err = micro_db_table_get(&s_settings_table, &k_settings_key, &current);
    if (err == ESP_ERR_NOT_FOUND) {
        gw_proto_settings_v1_t defaults = {
            .screensaver_timeout_ms = kDefaultScreensaverTimeoutMs,
            .weather_success_interval_ms = kDefaultWeatherSuccessIntervalMs,
            .weather_retry_interval_ms = kDefaultWeatherRetryIntervalMs,
            .timezone_auto = kDefaultTimezoneAuto,
            .timezone_offset_min = kDefaultTimezoneOffsetMin,
            .weather_location_auto = kDefaultWeatherLocationAuto,
        };
        return micro_db_table_upsert(&s_settings_table, &defaults, NULL, NULL);
    }

    return err;
}

esp_err_t gw_model_deinit_settings(void)
{
    return micro_db_table_deinit(&s_settings_table);
}

bool gw_model_settings_validate(const gw_proto_settings_v1_t *record)
{
    if (!record) {
        return false;
    }
    if (record->screensaver_timeout_ms < kMinScreensaverTimeoutMs ||
        record->screensaver_timeout_ms > kMaxScreensaverTimeoutMs) {
        return false;
    }
    if (record->weather_success_interval_ms < kMinWeatherSuccessIntervalMs ||
        record->weather_success_interval_ms > kMaxWeatherSuccessIntervalMs) {
        return false;
    }
    if (record->weather_retry_interval_ms < kMinWeatherRetryIntervalMs ||
        record->weather_retry_interval_ms > kMaxWeatherRetryIntervalMs) {
        return false;
    }
    if (record->timezone_offset_min < kMinTimezoneOffsetMin ||
        record->timezone_offset_min > kMaxTimezoneOffsetMin) {
        return false;
    }
    if (!record->weather_location_auto) {
        if (record->weather_lat < -90.0f || record->weather_lat > 90.0f) {
            return false;
        }
        if (record->weather_lon < -180.0f || record->weather_lon > 180.0f) {
            return false;
        }
        if (!record->weather_city[0]) {
            return false;
        }
    }
    return true;
}

esp_err_t gw_model_set_settings(const gw_proto_settings_v1_t *record,
                                bool *out_changed,
                                bool *out_inserted)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_proto_settings_v1_t trimmed = {0};
    gw_proto_trim_settings_v1(&trimmed, record);
    record = &trimmed;

    if (!gw_model_settings_validate(record)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool changed = false;
    bool inserted = false;
    esp_err_t err = micro_db_table_upsert(&s_settings_table,
                                          record,
                                          out_changed ? out_changed : &changed,
                                          out_inserted ? out_inserted : &inserted);
    if (err == ESP_OK && (changed || inserted)) {
        (void)gw_model_notify_settings(record);
    }
    return err;
}

esp_err_t gw_model_get_settings(gw_proto_settings_v1_t *out_record)
{
    if (!out_record) {
        return ESP_ERR_INVALID_ARG;
    }
    return micro_db_table_get(&s_settings_table, &k_settings_key, out_record);
}
