#include "gw_http/gw_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "gw_http/gw_rest.h"
#include "gw_http/gw_ws.h"

static const char *TAG = "gw_http";

static httpd_handle_t s_server;
static uint16_t s_server_port;
static bool s_spiffs_mounted;

static esp_err_t gw_http_spiffs_init(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info(conf.partition_label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted (%s): total=%u used=%u", conf.partition_label, (unsigned)total, (unsigned)used);
    }

    s_spiffs_mounted = true;
    return ESP_OK;
}

static const char *gw_http_content_type_from_path(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL) {
        return "text/plain";
    }
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".map") == 0) return "application/json";
    return "application/octet-stream";
}

static bool gw_http_uri_looks_like_asset(const char *uri)
{
    const char *slash = strrchr(uri, '/');
    const char *dot = strrchr(uri, '.');
    return (dot != NULL && (slash == NULL || dot > slash));
}

static esp_err_t gw_http_send_spiffs_file(httpd_req_t *req, const char *uri_path)
{
    if (!s_spiffs_mounted) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "web fs not mounted");
        return ESP_OK;
    }

    char fullpath[256];
    int n = snprintf(fullpath, sizeof(fullpath), "/www%s", uri_path);
    if (n <= 0 || n >= (int)sizeof(fullpath)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "path too long");
        return ESP_OK;
    }

    FILE *f = fopen(fullpath, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, gw_http_content_type_from_path(fullpath));

    uint8_t buf[1024];
    while (true) {
        size_t r = fread(buf, 1, sizeof(buf), f);
        if (r > 0) {
            esp_err_t err = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)r);
            if (err != ESP_OK) {
                fclose(f);
                httpd_resp_send_chunk(req, NULL, 0);
                return err;
            }
        }
        if (r < sizeof(buf)) {
            break;
        }
    }

    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    return gw_http_send_spiffs_file(req, "/index.html");
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) {
        return gw_http_send_spiffs_file(req, "/index.html");
    }

    // Strip query (defensive; typically not present in req->uri).
    const char *q = strchr(uri, '?');
    size_t uri_len = (q != NULL) ? (size_t)(q - uri) : strlen(uri);
    if (uri_len == 0 || uri_len > 200) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "bad uri");
        return ESP_OK;
    }

    char path[256];
    int n = snprintf(path, sizeof(path), "%.*s", (int)uri_len, uri);
    if (n <= 0 || n >= (int)sizeof(path)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "bad uri");
        return ESP_OK;
    }

    // If file exists -> serve it.
    char fullpath[256];
    n = snprintf(fullpath, sizeof(fullpath), "/www%s", path);
    if (n > 0 && n < (int)sizeof(fullpath)) {
        struct stat st;
        if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
            return gw_http_send_spiffs_file(req, path);
        }
    }

    // SPA fallback: if it's not an asset path, serve index.html for client-side routing.
    if (!gw_http_uri_looks_like_asset(path)) {
        return gw_http_send_spiffs_file(req, "/index.html");
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
}

esp_err_t gw_http_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    (void)gw_http_spiffs_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // Keep HTTP below Zigbee/rules so UI traffic cannot delay automations.
    config.task_priority = 4;
    // REST + WS + SPA assets can easily exceed the default handler slots.
    config.max_uri_handlers = 64;
    // WS/events and CBOR encoding can be stack-hungry in the httpd task.
    config.stack_size = 12288;
    s_server_port = config.server_port;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server_port = 0;
        return err;
    }

    static const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_get_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &index_uri));
    ESP_ERROR_CHECK(gw_http_register_rest_endpoints(s_server));
    ESP_ERROR_CHECK(gw_ws_register(s_server));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &static_uri));

    if (s_server_port != 0) {
        ESP_LOGI(TAG, "HTTP server started (port %u)", (unsigned)s_server_port);
    } else {
        ESP_LOGI(TAG, "HTTP server started");
    }
    return ESP_OK;
}

esp_err_t gw_http_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    gw_ws_unregister();
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    s_server_port = 0;
    return err;
}

uint16_t gw_http_get_port(void)
{
    return s_server_port;
}
