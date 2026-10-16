#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * HTTP GET + мини-разбор плоского JSON для сервиса system (geoip/geocode/weather).
 * Без внешнего парсера: ищем "key" и берём значение; вложенность не поддержана.
 */

esp_err_t system_http_get_text(const char *url, char *buf, size_t cap);

const char *system_json_find(const char *json, const char *key);
bool system_json_string(const char *json, const char *key, char *out, size_t out_size);
bool system_json_number(const char *json, const char *key, double *out);

/* percent-кодирование query (город может быть кириллицей). */
void system_url_encode(const char *in, char *out, size_t out_size);
