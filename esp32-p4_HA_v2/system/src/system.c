#include "system/system.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_system.h"
#include "ha_model/ha_zigbee.h"

/*
 * Сервис времени (docs/services/SYSTEM.md). Пока только время: SNTP даёт UTC, GeoIP —
 * пояс и город. Состояния системного девайса обновляются раз в минуту, события-«тики»
 * (MINUTE/HALF_HOUR/HOUR/DAY) публикуются фактом EVENT — на них Automation вешает правила.
 */

static const char *TAG = "system";

#define SYSTEM_TASK_STACK 6144
#define SYSTEM_TASK_PRIORITY 3

#define SYSTEM_NTP_SERVER "pool.ntp.org"
#define SYSTEM_SNTP_TIMEOUT_MS 10000

#define SYSTEM_GEOIP_URL \
    "http://ip-api.com/json/?fields=status,message,city,regionName,lat,lon,timezone,offset"
#define SYSTEM_GEOIP_TIMEOUT_MS 8000
#define SYSTEM_GEOIP_BODY_MAX 1024
#define SYSTEM_GEOIP_ATTEMPTS 5

static domain_t *s_domain;
static int32_t s_tz_offset_min;

/* --- сущности Domain ---------------------------------------------------- */

static void put_device(void)
{
    ha_device_record_t rec = {0};
    strlcpy(rec.name, "Время", sizeof(rec.name));
    strlcpy(rec.model, "Система", sizeof(rec.model));
    const ha_device_uid_t uid = HA_SYSTEM_DEVICE_UID;
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &rec, &meta, &changed);
}

static void put_endpoint(void)
{
    ha_endpoint_record_t rec = {0};
    rec.profile_id = HA_ZB_PROFILE_HA;
    rec.device_id = 0x0000;
    rec.cluster_count = 2;
    rec.clusters[0].cluster_id = HA_CLUSTER_TIME;
    rec.clusters[0].role = HA_ZB_ROLE_SERVER;
    rec.clusters[1].cluster_id = HA_CLUSTER_SYSTEM;
    rec.clusters[1].role = HA_ZB_ROLE_SERVER;

    const ha_endpoint_key_t key = {.device_uid = HA_SYSTEM_DEVICE_UID,
                                   .endpoint = HA_SYSTEM_ENDPOINT};
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_ENDPOINT, &key, &rec, &meta,
                            &changed);
}

/* Значение состояния хранится как в ZCL: raw + тип (масштаб — сторона читателя). */
static void put_state(uint16_t cluster_id, uint16_t attr_id, uint32_t raw, uint8_t zcl_type)
{
    const ha_zb_state_key_t key = {.device_uid = HA_SYSTEM_DEVICE_UID,
                                   .cluster_id = cluster_id,
                                   .attr_id = attr_id,
                                   .endpoint = HA_SYSTEM_ENDPOINT};
    const ha_zb_state_record_t rec = {.raw = raw, .zcl_type = zcl_type};
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_STATE, &key, &rec, &meta,
                            &changed);
}

static void put_location(const char *name, double lat, double lon, int32_t tz_offset_min)
{
    ha_location_record_t rec = {0};
    rec.latitude = (float)lat;
    rec.longitude = (float)lon;
    rec.tz_offset_min = (int16_t)tz_offset_min;
    strlcpy(rec.name, name ? name : "", sizeof(rec.name));

    const ha_device_uid_t uid = HA_SYSTEM_DEVICE_UID;
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_LOCATION, &uid, &rec, &meta,
                            &changed);
}

/* Событие системного девайса: факт EVENT, value.enum = event_id (payload не используется). */
static void publish_event(uint8_t event_id)
{
    const ha_device_uid_t uid = HA_SYSTEM_DEVICE_UID;
    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &uid,
    };
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    meta.value.type = (uint8_t)DOMAIN_VALUE_ENUM;
    meta.value.v.u32 = event_id;

    domain_payload_ref_t ref = 0;
    const uint8_t payload = event_id;
    (void)domain_payload_put(s_domain, &target, &meta, &payload, sizeof(payload), &ref);
}

/* --- сеть / время ------------------------------------------------------- */

static bool network_up(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

static void start_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SYSTEM_NTP_SERVER);
    cfg.start = true;
    const esp_err_t init_err = esp_netif_sntp_init(&cfg);
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(init_err));
        return;
    }
    const esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SYSTEM_SNTP_TIMEOUT_MS));
    ESP_LOGI(TAG, "sntp sync: %s", esp_err_to_name(err));
}

/* POSIX TZ использует обратный знак: UTC+3 => "UTC-3". IANA-имена newlib не знает. */
static void apply_tz(int32_t offset_sec)
{
    if (offset_sec < -18 * 3600 || offset_sec > 18 * 3600) {
        return;
    }
    const int32_t abs_off = (offset_sec >= 0) ? offset_sec : -offset_sec;
    const int hh = (int)(abs_off / 3600);
    const int mm = (int)((abs_off % 3600) / 60);
    const char sign = (offset_sec >= 0) ? '-' : '+';

    char tz[24] = {0};
    if (mm == 0) {
        (void)snprintf(tz, sizeof(tz), "UTC%c%d", sign, hh);
    } else {
        (void)snprintf(tz, sizeof(tz), "UTC%c%d:%02d", sign, hh, mm);
    }
    if (setenv("TZ", tz, 1) == 0) {
        tzset();
        ESP_LOGI(TAG, "timezone applied: %s (offset %ld s)", tz, (long)offset_sec);
    }
}

/* --- HTTP --------------------------------------------------------------- */

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

static esp_err_t http_get_text(const char *url, char *buf, size_t cap)
{
    http_buf_t out = {.buf = buf, .cap = cap, .len = 0};
    if (cap > 0) {
        buf[0] = '\0';
    }
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = SYSTEM_GEOIP_TIMEOUT_MS,
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

/*
 * Мини-извлечение полей из плоского JSON (ip-api): без внешнего парсера. Ищем
 * "key", затем значение. Достаточно для известных ответов; вложенность не поддержана.
 */
static const char *json_find(const char *json, const char *key)
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

static bool json_string(const char *json, const char *key, char *out, size_t out_size)
{
    const char *p = json_find(json, key);
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

static bool json_number(const char *json, const char *key, double *out)
{
    const char *p = json_find(json, key);
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

/* GeoIP: город + координаты + смещение пояса. Один вызов на старте. */
static bool fetch_location(char *city, size_t city_size, double *lat, double *lon, int32_t *offset_sec)
{
    static char body[SYSTEM_GEOIP_BODY_MAX];
    if (http_get_text(SYSTEM_GEOIP_URL, body, sizeof(body)) != ESP_OK) {
        ESP_LOGW(TAG, "geoip http failed");
        return false;
    }

    char status[16] = {0};
    if (!json_string(body, "status", status, sizeof(status)) || strcmp(status, "success") != 0) {
        ESP_LOGW(TAG, "geoip status: %s", status[0] ? status : "?");
        return false;
    }
    if (!json_number(body, "lat", lat) || !json_number(body, "lon", lon)) {
        return false;
    }

    double off = 0.0;
    if (json_number(body, "offset", &off)) {
        *offset_sec = (int32_t)off;
    }

    char c[32] = {0};
    char r[32] = {0};
    json_string(body, "city", c, sizeof(c));
    json_string(body, "regionName", r, sizeof(r));
    if (c[0] && r[0]) {
        (void)snprintf(city, city_size, "%s, %s", c, r);
    } else if (c[0]) {
        strlcpy(city, c, city_size);
    } else if (r[0]) {
        strlcpy(city, r, city_size);
    }
    return true;
}

static void resolve_location(void)
{
    char city[64] = {0};
    double lat = 0.0;
    double lon = 0.0;
    int32_t offset_sec = 0;

    for (int attempt = 1; attempt <= SYSTEM_GEOIP_ATTEMPTS; attempt++) {
        if (fetch_location(city, sizeof(city), &lat, &lon, &offset_sec)) {
            apply_tz(offset_sec);
            s_tz_offset_min = offset_sec / 60;
            put_location(city[0] ? city : "Unknown", lat, lon, s_tz_offset_min);
            ESP_LOGI(TAG, "location: %s (%.4f, %.4f)", city[0] ? city : "?", lat, lon);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_LOGW(TAG, "geoip unavailable; keeping UTC");
    put_location("Unknown", 0.0, 0.0, 0);
}

/* --- состояния / события ------------------------------------------------ */

static void update_time_states(const struct tm *tmv)
{
    const uint8_t weekday = (uint8_t)((tmv->tm_wday + 6) % 7); /* 0=Пн .. 6=Вс */
    const uint8_t weekday_mask = (uint8_t)(1u << weekday);
    const uint32_t minutes_of_day = (uint32_t)(tmv->tm_hour * 60 + tmv->tm_min);

    put_state(HA_CLUSTER_TIME, HA_TIME_ATTR_UTC_TIME, (uint32_t)time(NULL), HA_ZB_TYPE_UINT32);
    put_state(HA_CLUSTER_TIME, HA_TIME_ATTR_TIME_STATUS, 0x02u, HA_ZB_TYPE_BITMAP8);
    put_state(HA_CLUSTER_SYSTEM, HA_SYS_ATTR_HOUR, (uint32_t)tmv->tm_hour, HA_ZB_TYPE_UINT8);
    put_state(HA_CLUSTER_SYSTEM, HA_SYS_ATTR_MINUTE, (uint32_t)tmv->tm_min, HA_ZB_TYPE_UINT8);
    put_state(HA_CLUSTER_SYSTEM, HA_SYS_ATTR_WEEKDAY, weekday, HA_ZB_TYPE_UINT8);
    put_state(HA_CLUSTER_SYSTEM, HA_SYS_ATTR_WEEKDAY_MASK, weekday_mask, HA_ZB_TYPE_BITMAP8);
    put_state(HA_CLUSTER_SYSTEM, HA_SYS_ATTR_MINUTES_OF_DAY, minutes_of_day, HA_ZB_TYPE_UINT16);
    put_state(HA_CLUSTER_SYSTEM, HA_SYS_ATTR_TZ_OFFSET_MIN, (uint32_t)(int32_t)s_tz_offset_min,
              HA_ZB_TYPE_INT16);
}

static void emit_ticks(const struct tm *tmv)
{
    publish_event((uint8_t)HA_SYS_EVENT_MINUTE_TICK);
    if (tmv->tm_min % 30 == 0) {
        publish_event((uint8_t)HA_SYS_EVENT_HALF_HOUR_TICK);
    }
    if (tmv->tm_min == 0) {
        publish_event((uint8_t)HA_SYS_EVENT_HOUR_TICK);
        if (tmv->tm_hour == 0) {
            publish_event((uint8_t)HA_SYS_EVENT_DAY_TICK);
        }
    }
}

/* --- задача ------------------------------------------------------------- */

static void system_task(void *arg)
{
    (void)arg;

    while (!network_up()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "network up; syncing time");
    start_sntp();

    put_device();
    put_endpoint();
    resolve_location();

    for (;;) {
        const time_t now = time(NULL);
        struct tm tmv = {0};
        localtime_r(&now, &tmv);

        update_time_states(&tmv);
        emit_ticks(&tmv);

        /* Спим до начала следующей минуты, чтобы «тики» били ровно по границе. */
        const uint32_t delay_ms = (uint32_t)((60 - tmv.tm_sec) * 1000);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

sys_error_t system_start(domain_t *domain)
{
    if (domain == NULL) {
        return sys_error_make(SYS_LAYER_SYSTEM, SYS_CODE_INVALID_ARG);
    }
    s_domain = domain;
    if (xTaskCreate(system_task, "system", SYSTEM_TASK_STACK, NULL, SYSTEM_TASK_PRIORITY, NULL) !=
        pdPASS) {
        return sys_error_make(SYS_LAYER_SYSTEM, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
