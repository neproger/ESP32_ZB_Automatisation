#include "gw_core/net_fetch.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "gw_net_fetch";

static const int kDefaultTimeoutMs = 7000;
static const int kDefaultMaxBodyBytes = 4096;

static const cJSON *json_get_by_path(const cJSON *root, const char *path)
{
    if (!root || !path || path[0] == '\0') {
        return NULL;
    }

    const cJSON *node = root;
    char token[64];
    size_t ti = 0;
    for (const char *p = path;; p++) {
        const char ch = *p;
        const bool sep = (ch == '.' || ch == '\0');
        if (!sep) {
            if (ti + 1 >= sizeof(token)) {
                return NULL;
            }
            token[ti++] = ch;
            continue;
        }

        if (ti == 0) {
            return NULL;
        }
        token[ti] = '\0';
        node = cJSON_GetObjectItemCaseSensitive((cJSON *)node, token);
        if (!node) {
            return NULL;
        }
        ti = 0;
        if (ch == '\0') {
            return node;
        }
    }
}

esp_err_t gw_net_fetch_get_text(const char *url, const gw_net_fetch_cfg_t *cfg, char *out, size_t out_size, int *out_http_status)
{
    if (!url || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (out_http_status) {
        *out_http_status = 0;
    }

    const int timeout_ms = (cfg && cfg->timeout_ms > 0) ? cfg->timeout_ms : kDefaultTimeoutMs;
    const int max_body = (cfg && cfg->max_body_bytes > 0) ? cfg->max_body_bytes : kDefaultMaxBodyBytes;
    if ((int)out_size < 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .buffer_size = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (out_http_status) {
        *out_http_status = status;
    }

    size_t total = 0;
    int read_n = 0;
    do {
        char chunk[256];
        read_n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read_n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read_n == 0) {
            break;
        }
        if ((int)(total + (size_t)read_n) > max_body || total + (size_t)read_n + 1 > out_size) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        memcpy(out + total, chunk, (size_t)read_n);
        total += (size_t)read_n;
    } while (read_n > 0);

    out[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t gw_net_fetch_get_json_number(const char *url, const gw_net_fetch_cfg_t *cfg, const char *json_path, double *out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    char body[4096];
    int status = 0;
    esp_err_t err = gw_net_fetch_get_text(url, cfg, body, sizeof(body), &status);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *node = json_get_by_path(root, json_path);
    if (!node || !cJSON_IsNumber(node)) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    *out_value = node->valuedouble;
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t gw_net_fetch_get_json_text(const char *url, const gw_net_fetch_cfg_t *cfg, const char *json_path, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    char body[4096];
    int status = 0;
    esp_err_t err = gw_net_fetch_get_text(url, cfg, body, sizeof(body), &status);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *node = json_get_by_path(root, json_path);
    if (!node || !cJSON_IsString(node) || !node->valuestring) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    const size_t n = strlen(node->valuestring);
    if (n + 1 > out_size) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, node->valuestring, n + 1);
    cJSON_Delete(root);
    return ESP_OK;
}
