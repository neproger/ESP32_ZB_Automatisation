#include "s3_weather_http.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "s3_http_client.h"

static const char *TAG = "s3_weather_http";
static const char *kWeatherBaseUrl = "http://api.open-meteo.com/v1/forecast";

static esp_err_t set_error(char *out_error, size_t out_error_size, const char *msg)
{
    if (out_error && out_error_size > 0) {
        (void)snprintf(out_error, out_error_size, "%s", msg ? msg : "unknown");
    }
    return ESP_FAIL;
}

esp_err_t s3_weather_http_fetch_once(double latitude,
                                     double longitude,
                                     int timeout_ms,
                                     s3_weather_result_t *out_result,
                                     char *out_error,
                                     size_t out_error_size)
{
    if (!out_result) {
        return set_error(out_error, out_error_size, "out_result is null");
    }
    if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) {
        return set_error(out_error, out_error_size, "invalid lat/lon");
    }

    memset(out_result, 0, sizeof(*out_result));
    if (out_error && out_error_size > 0) {
        out_error[0] = '\0';
    }

    char url[512];
    int n = snprintf(url,
                     sizeof(url),
                     "%s?latitude=%.6f&longitude=%.6f&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto",
                     kWeatherBaseUrl,
                     latitude,
                     longitude);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return set_error(out_error, out_error_size, "url too long");
    }

    ESP_LOGI(TAG, "weather request start: %s", url);
    char *body = NULL;
    esp_err_t err = s3_http_client_get_text(url, timeout_ms, 4096, &body, NULL, out_error, out_error_size);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return set_error(out_error, out_error_size, "json parse failed");
    }

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *temperature = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "temperature_2m") : NULL;
    const cJSON *humidity = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "relative_humidity_2m") : NULL;
    const cJSON *wind = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "wind_speed_10m") : NULL;
    const cJSON *code = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "weather_code") : NULL;
    const cJSON *obs_time = current ? cJSON_GetObjectItemCaseSensitive((cJSON *)current, "time") : NULL;

    if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(humidity) || !cJSON_IsNumber(wind) || !cJSON_IsNumber(code)) {
        cJSON_Delete(root);
        return set_error(out_error, out_error_size, "json schema mismatch");
    }

    out_result->valid = true;
    out_result->temperature_c = (float)temperature->valuedouble;
    out_result->humidity_pct = (float)humidity->valuedouble;
    out_result->wind_speed_kmh = (float)wind->valuedouble;
    out_result->weather_code = code->valueint;
    if (cJSON_IsString(obs_time) && obs_time->valuestring) {
        strlcpy(out_result->observed_time, obs_time->valuestring, sizeof(out_result->observed_time));
    } else {
        out_result->observed_time[0] = '\0';
    }

    cJSON_Delete(root);
    return ESP_OK;
}

