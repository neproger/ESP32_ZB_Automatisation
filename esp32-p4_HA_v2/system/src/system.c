#include "system/system.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_entity.h"
#include "ha_model/ha_properties.h"
#include "ha_model/ha_state.h"
#include "ha_model/ha_system.h"
#include "ha_model/ha_weather.h"

#include "system_http.h"
#include "system_weather.h"

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
#define SYSTEM_GEOIP_BODY_MAX 1024
#define SYSTEM_GEOIP_ATTEMPTS 5

#define SYSTEM_WEATHER_PERIOD_MIN 15
#define SYSTEM_WEATHER_ATTEMPTS 3

static domain_t *s_domain;
static int32_t s_tz_offset_min;

static double s_lat;
static double s_lon;
static bool s_weather_valid;
static uint8_t s_weather_condition;
static ha_location_record_t s_last_location;
static bool s_have_last_location;

/* --- сущности Domain ---------------------------------------------------- */

/* System сам владеет семантикой своих величин (ZCL его не касается): canonical напрямую. */
static ha_value_t u32_value(uint32_t v)
{
    ha_value_t value = {0};
    value.kind = HA_VALUE_U32;
    value.value.u32 = v;
    return value;
}

static ha_value_t i32_value(int32_t v)
{
    ha_value_t value = {0};
    value.kind = HA_VALUE_I32;
    value.value.i32 = v;
    return value;
}

static void put_state(ha_property_id_t property, ha_value_t value)
{
    const ha_state_key_t key = {
        .entity = ha_entity_singleton(HA_ENTITY_ISSUER_SYSTEM),
        .property = property,
    };
    const ha_state_record_t rec = {.value = value};
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &key, &rec, &meta,
                            &changed);
}

static bool get_location(ha_location_record_t *out)
{
    const ha_device_id_t uid = HA_SYSTEM_DEVICE_ID;
    return sys_ok(
        domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_LOCATION, &uid, out));
}

static void put_location_record(const ha_location_record_t *rec)
{
    const ha_device_id_t uid = HA_SYSTEM_DEVICE_ID;
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_LOCATION, &uid, rec, &meta,
                            &changed);
}

static void put_location(const char *name, double lat, double lon, int32_t tz_offset_min,
                         uint8_t flags)
{
    ha_location_record_t rec = {0};
    rec.latitude = (float)lat;
    rec.longitude = (float)lon;
    rec.tz_offset_min = (int16_t)tz_offset_min;
    rec.flags = flags;
    strlcpy(rec.name, name ? name : "", sizeof(rec.name));
    put_location_record(&rec);
}

/*
 * Событие системного девайса. A3.1: semantic event адресуется источником-Entity
 * (ENTITY_SYSTEM), а не физическим uid. Legacy DEVICE-факт публикуется параллельно до A3.3.
 */
static void publish_event(uint8_t event_id)
{
    const ha_entity_id_t source = ha_entity_singleton(HA_ENTITY_ISSUER_SYSTEM);
    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_ENTITY,
        .key = &source,
    };
    const ha_event_t event = {.id = (ha_event_id_t)event_id, .source_entity = source, .value = {0}};
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    domain_payload_ref_t ref = 0;
    (void)domain_payload_put(s_domain, &target, &meta, &event, sizeof(event), &ref);
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

/* Город → координаты (Open-Meteo geocoding): ручное место задаётся именем, без lat/lon. */
static bool geocode_city(const char *city, double *lat, double *lon)
{
    static char body[1024];
    char enc[128] = {0};
    char url[256];
    system_url_encode(city, enc, sizeof(enc));
    const int n = snprintf(url, sizeof(url),
                           "http://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&"
                           "language=en&format=json",
                           enc);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return false;
    }
    if (system_http_get_text(url, body, sizeof(body)) != ESP_OK) {
        ESP_LOGW(TAG, "geocode http failed");
        return false;
    }
    const char *results = strstr(body, "\"results\":");
    if (results == NULL) {
        ESP_LOGW(TAG, "geocode: нет results для \"%s\"", city);
        return false;
    }
    return system_json_number(results, "latitude", lat) && system_json_number(results, "longitude", lon);
}

/* GeoIP: город + координаты + смещение пояса. Один вызов на старте. */
static bool fetch_location(char *city, size_t city_size, double *lat, double *lon, int32_t *offset_sec)
{
    static char body[SYSTEM_GEOIP_BODY_MAX];
    if (system_http_get_text(SYSTEM_GEOIP_URL, body, sizeof(body)) != ESP_OK) {
        ESP_LOGW(TAG, "geoip http failed");
        return false;
    }

    char status[16] = {0};
    if (!system_json_string(body, "status", status, sizeof(status)) || strcmp(status, "success") != 0) {
        ESP_LOGW(TAG, "geoip status: %s", status[0] ? status : "?");
        return false;
    }
    if (!system_json_number(body, "lat", lat) || !system_json_number(body, "lon", lon)) {
        return false;
    }

    double off = 0.0;
    if (system_json_number(body, "offset", &off)) {
        *offset_sec = (int32_t)off;
    }

    char c[32] = {0};
    char r[32] = {0};
    system_json_string(body, "city", c, sizeof(c));
    system_json_string(body, "regionName", r, sizeof(r));
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
    ha_location_record_t loc = {0};
    const bool have = get_location(&loc);
    const bool pos_manual =
        have && (loc.flags & HA_LOCATION_FLAG_POS_AUTO) == 0 && loc.name[0] != '\0';
    const bool tz_manual = have && (loc.flags & HA_LOCATION_FLAG_TZ_AUTO) == 0;

    /* Ручное место: имя (+ координаты, если есть; иначе геокодируем). */
    if (pos_manual) {
        double lat = loc.latitude;
        double lon = loc.longitude;
        if (fabs(lat) < 1e-4 && fabs(lon) < 1e-4) {
            if (geocode_city(loc.name, &lat, &lon)) {
                loc.latitude = (float)lat;
                loc.longitude = (float)lon;
            }
        }
        s_lat = lat;
        s_lon = lon;
        if (tz_manual) {
            s_tz_offset_min = loc.tz_offset_min;
            apply_tz((int32_t)loc.tz_offset_min * 60);
        } else {
            char city[64] = {0};
            double glat = 0.0, glon = 0.0;
            int32_t off = 0;
            if (fetch_location(city, sizeof(city), &glat, &glon, &off)) {
                s_tz_offset_min = off / 60;
                apply_tz(off);
            }
            loc.flags |= HA_LOCATION_FLAG_TZ_AUTO;
        }
        put_location_record(&loc);
        ESP_LOGI(TAG, "location (manual): %s (%.4f, %.4f) tz=%d", loc.name, s_lat, s_lon,
                 (int)s_tz_offset_min);
        return;
    }

    /* Авто-место из GeoIP. */
    char city[64] = {0};
    double lat = 0.0;
    double lon = 0.0;
    int32_t offset_sec = 0;
    for (int attempt = 1; attempt <= SYSTEM_GEOIP_ATTEMPTS; attempt++) {
        if (fetch_location(city, sizeof(city), &lat, &lon, &offset_sec)) {
            s_lat = lat;
            s_lon = lon;
            if (tz_manual) {
                s_tz_offset_min = loc.tz_offset_min;
                apply_tz((int32_t)s_tz_offset_min * 60);
            } else {
                s_tz_offset_min = offset_sec / 60;
                apply_tz(offset_sec);
            }
            const uint8_t flags =
                HA_LOCATION_FLAG_POS_AUTO | (tz_manual ? 0u : HA_LOCATION_FLAG_TZ_AUTO);
            put_location(city[0] ? city : "Unknown", lat, lon, s_tz_offset_min, flags);
            ESP_LOGI(TAG, "location: %s (%.4f, %.4f)", city[0] ? city : "?", lat, lon);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_LOGW(TAG, "geoip unavailable; keeping UTC");
    put_location("Unknown", 0.0, 0.0, 0, HA_LOCATION_FLAG_POS_AUTO | HA_LOCATION_FLAG_TZ_AUTO);
}

static void put_weather(const ha_weather_record_t *rec)
{
    const ha_device_id_t uid = HA_WEATHER_DEVICE_ID;
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_WEATHER, &uid, rec, &meta, &changed);
}

static void refresh_weather(void)
{
    ha_weather_record_t rec = {0};
    for (int attempt = 1; attempt <= SYSTEM_WEATHER_ATTEMPTS; attempt++) {
        if (system_weather_fetch(s_lat, s_lon, &rec)) {
            put_weather(&rec);
            const bool changed = s_weather_valid && rec.condition != s_weather_condition;
            s_weather_valid = true;
            s_weather_condition = rec.condition;
            if (changed) {
                publish_event((uint8_t)HA_EVENT_WEATHER_CHANGED);
            }
            ESP_LOGI(TAG, "weather: cond=%u t=%.1fC hum=%u%% wind=%.1fkm/h",
                     (unsigned)rec.condition, (double)rec.temperature_c100 / 100.0,
                     (unsigned)rec.humidity_p100 / 100, (double)rec.wind_kmh10 / 10.0);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "weather unavailable");
}

/* --- состояния / события ------------------------------------------------ */

static void update_time_states(const struct tm *tmv)
{
    const uint8_t weekday = (uint8_t)((tmv->tm_wday + 6) % 7); /* 0=Пн .. 6=Вс */
    const uint8_t weekday_mask = (uint8_t)(1u << weekday);
    const uint32_t minutes_of_day = (uint32_t)(tmv->tm_hour * 60 + tmv->tm_min);

    put_state(HA_PROPERTY_SYSTEM_TIME_UTC, u32_value((uint32_t)time(NULL)));
    put_state(HA_PROPERTY_SYSTEM_TIME_STATUS, u32_value(0x02u));
    put_state(HA_PROPERTY_SYSTEM_HOUR, u32_value((uint32_t)tmv->tm_hour));
    put_state(HA_PROPERTY_SYSTEM_MINUTE, u32_value((uint32_t)tmv->tm_min));
    put_state(HA_PROPERTY_SYSTEM_WEEKDAY, u32_value(weekday));
    put_state(HA_PROPERTY_SYSTEM_WEEKDAY_MASK, u32_value(weekday_mask));
    put_state(HA_PROPERTY_SYSTEM_MINUTES_OF_DAY, u32_value(minutes_of_day));
    put_state(HA_PROPERTY_SYSTEM_TZ_OFFSET, i32_value(s_tz_offset_min));
}

static void emit_ticks(const struct tm *tmv)
{
    publish_event((uint8_t)HA_EVENT_MINUTE_TICK);
    if (tmv->tm_min % 30 == 0) {
        publish_event((uint8_t)HA_EVENT_HALF_HOUR_TICK);
    }
    if (tmv->tm_min == 0) {
        publish_event((uint8_t)HA_EVENT_HOUR_TICK);
        if (tmv->tm_hour == 0) {
            publish_event((uint8_t)HA_EVENT_DAY_TICK);
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

    resolve_location();
    refresh_weather();
    s_have_last_location = get_location(&s_last_location);

    uint32_t minutes_since_weather = 0;
    for (;;) {
        const time_t now = time(NULL);
        struct tm tmv = {0};
        localtime_r(&now, &tmv);

        update_time_states(&tmv);
        emit_ticks(&tmv);

        if (++minutes_since_weather >= SYSTEM_WEATHER_PERIOD_MIN) {
            minutes_since_weather = 0;
            refresh_weather();
        }

        /* Спим до начала следующей минуты, но мелкими шагами: ручная правка места/пояса
         * (web) применяется за считанные секунды, а не раз в минуту. */
        uint32_t remain_ms = (uint32_t)((60 - tmv.tm_sec) * 1000);
        while (remain_ms > 0) {
            const uint32_t chunk_ms = remain_ms > 5000 ? 5000 : remain_ms;
            vTaskDelay(pdMS_TO_TICKS(chunk_ms));
            remain_ms -= chunk_ms;

            ha_location_record_t loc = {0};
            if (get_location(&loc) &&
                (!s_have_last_location || memcmp(&loc, &s_last_location, sizeof(loc)) != 0)) {
                resolve_location();
                (void)get_location(&s_last_location); /* запоминаем то, что записал system */
                s_have_last_location = true;
                refresh_weather();
            }
        }
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
