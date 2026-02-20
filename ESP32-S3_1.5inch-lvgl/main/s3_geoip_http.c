#include "s3_geoip_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"

typedef struct
{
    char *buf;
    size_t cap;
    size_t len;
} http_buf_t;

static esp_err_t set_error(char *out_error, size_t out_error_size, const char *msg)
{
    if (out_error && out_error_size > 0) {
        (void)snprintf(out_error, out_error_size, "%s", msg ? msg : "unknown");
    }
    return ESP_FAIL;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (!evt || !evt->user_data) {
        return ESP_OK;
    }

    http_buf_t *ctx = (http_buf_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t n = (size_t)evt->data_len;
    if (ctx->len + n >= ctx->cap) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(ctx->buf + ctx->len, evt->data, n);
    ctx->len += n;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
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
    char *body = (char *)calloc(1, 2048);
    if (!body) {
        return set_error(out_error, out_error_size, "no mem for response");
    }

    http_buf_t buf = {
        .buf = body,
        .cap = 2048,
        .len = 0,
    };

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : 8000,
        .event_handler = http_event_handler,
        .user_data = &buf,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        return set_error(out_error, out_error_size, "esp_http_client_init failed");
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(body);
        if (out_error && out_error_size > 0) {
            (void)snprintf(out_error, out_error_size, "http perform failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status != 200) {
        free(body);
        if (out_error && out_error_size > 0) {
            (void)snprintf(out_error, out_error_size, "http status=%d", status);
        }
        return ESP_ERR_INVALID_RESPONSE;
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
