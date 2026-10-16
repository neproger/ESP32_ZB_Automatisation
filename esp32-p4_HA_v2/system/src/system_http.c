#include "system_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"

#define SYSTEM_HTTP_TIMEOUT_MS 8000

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} http_buf_t;

static esp_err_t http_on_data(esp_http_client_event_t *evt)
{
    http_buf_t *out = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && out != NULL && evt->data_len > 0) {
        if (out->len + (size_t)evt->data_len >= out->cap) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(out->buf + out->len, evt->data, evt->data_len);
        out->len += (size_t)evt->data_len;
        out->buf[out->len] = '\0';
    }
    return ESP_OK;
}

esp_err_t system_http_get_text(const char *url, char *buf, size_t cap)
{
    http_buf_t out = {.buf = buf, .cap = cap, .len = 0};
    if (cap > 0) {
        buf[0] = '\0';
    }
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = SYSTEM_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_GET,
        .event_handler = http_on_data,
        .user_data = &out,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && esp_http_client_get_status_code(client) != 200) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    esp_http_client_cleanup(client);
    return err;
}

const char *system_json_find(const char *json, const char *key)
{
    char pattern[48];
    const int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) {
        return NULL;
    }
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == ':') {
        p++;
    }
    return p;
}

bool system_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    const char *p = system_json_find(json, key);
    if (p == NULL || *p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (*p != '\0' && *p != '"' && (n + 1) < out_size) {
        if (*p == '\\' && p[1] != '\0') {
            p++; /* простейшая разэкранировка */
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

bool system_json_number(const char *json, const char *key, double *out)
{
    const char *p = system_json_find(json, key);
    if (p == NULL) {
        return false;
    }
    char *end = NULL;
    const double value = strtod(p, &end);
    if (end == p) {
        return false;
    }
    *out = value;
    return true;
}

void system_url_encode(const char *in, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0' && n + 4 < out_size; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[n++] = (char)*p;
        } else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 0x0F];
        }
    }
    out[n] = '\0';
}
