#include "s3_http_client.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"

typedef struct
{
    char *buf;
    size_t cap;
    size_t len;
} s3_http_buf_t;

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

    s3_http_buf_t *ctx = (s3_http_buf_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    const size_t n = (size_t)evt->data_len;
    if (ctx->len + n >= ctx->cap) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(ctx->buf + ctx->len, evt->data, n);
    ctx->len += n;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

esp_err_t s3_http_client_get_text(const char *url,
                                  int timeout_ms,
                                  size_t max_body_size,
                                  char **out_body,
                                  int *out_http_status,
                                  char *out_error,
                                  size_t out_error_size)
{
    if (!url || !url[0] || !out_body || max_body_size < 2) {
        return set_error(out_error, out_error_size, "invalid args");
    }

    *out_body = NULL;
    if (out_http_status) {
        *out_http_status = 0;
    }
    if (out_error && out_error_size > 0) {
        out_error[0] = '\0';
    }

    char *body = (char *)calloc(1, max_body_size);
    if (!body) {
        return set_error(out_error, out_error_size, "no mem for response");
    }

    s3_http_buf_t buf = {
        .buf = body,
        .cap = max_body_size,
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

    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (out_http_status) {
        *out_http_status = status;
    }
    if (status != 200) {
        free(body);
        if (out_error && out_error_size > 0) {
            (void)snprintf(out_error, out_error_size, "http status=%d", status);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_body = body;
    return ESP_OK;
}
