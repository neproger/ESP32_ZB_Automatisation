#include "s3_geoip_http.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "s3_http_client.h"

static esp_err_t set_error(char *out_error, size_t out_error_size, const char *msg)
{
    if (out_error && out_error_size > 0) {
        (void)snprintf(out_error, out_error_size, "%s", msg ? msg : "unknown");
    }
    return ESP_FAIL;
}

esp_err_t s3_geoip_http_fetch_once(int timeout_ms, s3_geoip_result_t *out_result, char *out_error, size_t out_error_size)
{
    if (!out_result) {
        return set_error(out_error, out_error_size, "out_result is null");
    }

    memset(out_result, 0, sizeof(*out_result));
    if (out_error && out_error_size > 0) {
        out_error[0] = '\0';
    }

    const char *url = "http://ip-api.com/json/?fields=status,message,city,regionName,lat,lon,timezone,offset";
    char *body = NULL;
    esp_err_t err = s3_http_client_get_text(url, timeout_ms, 2048, &body, NULL, out_error, out_error_size);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return set_error(out_error, out_error_size, "json parse failed");
    }

    const cJSON *status_field = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!cJSON_IsString(status_field) || !status_field->valuestring || strcmp(status_field->valuestring, "success") != 0) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (out_error && out_error_size > 0 && cJSON_IsString(msg) && msg->valuestring) {
            (void)snprintf(out_error, out_error_size, "geoip failed: %s", msg->valuestring);
        }
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");
    const cJSON *region = cJSON_GetObjectItemCaseSensitive(root, "regionName");
    const cJSON *timezone = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(root, "offset");
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(root, "lat");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(root, "lon");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
        cJSON_Delete(root);
        return set_error(out_error, out_error_size, "geoip missing lat/lon");
    }

    out_result->valid = true;
    out_result->latitude = lat->valuedouble;
    out_result->longitude = lon->valuedouble;
    if (cJSON_IsNumber(offset)) {
        out_result->utc_offset_sec = (int32_t)offset->valuedouble;
    }
    if (cJSON_IsString(city) && city->valuestring) {
        strlcpy(out_result->city, city->valuestring, sizeof(out_result->city));
    }
    if (cJSON_IsString(region) && region->valuestring) {
        strlcpy(out_result->region, region->valuestring, sizeof(out_result->region));
    }
    if (cJSON_IsString(timezone) && timezone->valuestring) {
        strlcpy(out_result->timezone, timezone->valuestring, sizeof(out_result->timezone));
    }

    cJSON_Delete(root);
    return ESP_OK;
}
