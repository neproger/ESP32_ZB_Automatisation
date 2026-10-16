#include "system_weather.h"

#include <math.h>
#include <stdio.h>

#include "esp_log.h"
#include "system_http.h"

static const char *TAG = "system";

#define SYSTEM_WEATHER_URL "http://api.open-meteo.com/v1/forecast"
#define SYSTEM_WEATHER_BODY_MAX 2048

/* WMO weather_code → условие (ha_weather_condition_t). is_day — из поля is_day. */
static uint8_t wmo_to_condition(int code, bool is_day)
{
    switch (code) {
    case 0:
    case 1:
        return is_day ? (uint8_t)HA_WEATHER_CLEAR : (uint8_t)HA_WEATHER_CLEAR_NIGHT;
    case 2:
        return (uint8_t)HA_WEATHER_PARTLYCLOUDY;
    case 3:
        return (uint8_t)HA_WEATHER_OVERCAST;
    case 45:
    case 48:
        return (uint8_t)HA_WEATHER_FOG;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
    case 61:
    case 63:
    case 65:
        return (uint8_t)HA_WEATHER_RAINY;
    case 66:
    case 67:
        return (uint8_t)HA_WEATHER_SNOWY_RAINY;
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return (uint8_t)HA_WEATHER_SNOWY;
    case 80:
    case 81:
    case 82:
        return (uint8_t)HA_WEATHER_POURING;
    case 95:
        return (uint8_t)HA_WEATHER_LIGHTNING;
    case 96:
    case 99:
        return (uint8_t)HA_WEATHER_LIGHTNING_RAINY;
    default:
        return (uint8_t)HA_WEATHER_UNKNOWN;
    }
}

bool system_weather_fetch(double lat, double lon, ha_weather_record_t *out)
{
    static char body[SYSTEM_WEATHER_BODY_MAX];
    char url[320];
    const int n = snprintf(url, sizeof(url),
                           "%s?latitude=%.4f&longitude=%.4f&current=temperature_2m,"
                           "relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m,"
                           "cloud_cover,surface_pressure,is_day",
                           SYSTEM_WEATHER_URL, lat, lon);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return false;
    }
    if (system_http_get_text(url, body, sizeof(body)) != ESP_OK) {
        ESP_LOGW(TAG, "weather http failed");
        return false;
    }

    /* Ключи есть и в `current_units` (строки) — берём только внутри `current`. */
    const char *current = strstr(body, "\"current\":");
    if (current == NULL) {
        ESP_LOGW(TAG, "weather json: нет объекта current");
        return false;
    }

    double temp = 0.0;
    double code = 0.0;
    if (!system_json_number(current, "temperature_2m", &temp) ||
        !system_json_number(current, "weather_code", &code)) {
        ESP_LOGW(TAG, "weather json: нет temperature_2m/weather_code");
        return false;
    }

    ha_weather_record_t rec = {0};
    double v = 0.0;
    const bool is_day = !(system_json_number(current, "is_day", &v) && v == 0.0);
    rec.condition = wmo_to_condition((int)code, is_day);
    rec.temperature_c100 = (int16_t)lround(temp * 100.0);
    if (system_json_number(current, "relative_humidity_2m", &v)) {
        rec.humidity_p100 = (uint16_t)lround(v * 100.0);
    }
    if (system_json_number(current, "surface_pressure", &v)) {
        rec.pressure_hpa = (uint16_t)lround(v);
    }
    if (system_json_number(current, "wind_speed_10m", &v)) {
        rec.wind_kmh10 = (uint16_t)lround(v * 10.0);
    }
    if (system_json_number(current, "wind_direction_10m", &v)) {
        rec.wind_dir_deg = (uint16_t)lround(v);
    }
    if (system_json_number(current, "cloud_cover", &v)) {
        rec.cloud_pct = (uint8_t)lround(v);
    }
    *out = rec;
    return true;
}
