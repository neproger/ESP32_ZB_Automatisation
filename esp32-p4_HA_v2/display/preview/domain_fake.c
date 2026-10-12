/*
 * Фейковый Domain для host-превью (LVGL Live Preview): тот же UI-код, но данные —
 * мок. Реализованы только функции Domain, которые зовёт ui/. На прошивке линкуется
 * настоящий Domain (docs/clients/DISPLAY.md).
 *
 * Модель canonical (Шаг A): состояние — PROPERTY_STATE (entity, property); group_item
 * ссылается на canonical-свойство. Raw Zigbee в Domain больше не попадает (Шаг C).
 */

#include <stdbool.h>
#include <string.h>

#include "domain/domain.h"
#include "ha_model/ha_capabilities.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_entity.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_logical_device.h"
#include "ha_model/ha_properties.h"
#include "ha_model/ha_settings.h"
#include "ha_model/ha_state.h"
#include "ha_model/ha_system.h"
#include "ha_model/ha_weather.h"
#include "ha_model/ha_wifi.h"

/* Синтетические Entity превью (обычно выводятся adapter'ом; для мока — фиксированные). */
#define ENT_LIGHT 0x00124B0000A10001ull
#define ENT_SENSOR 0x00124B0000A10002ull
#define ENT_SYSTEM ha_entity_singleton(HA_ENTITY_ISSUER_SYSTEM)

typedef struct {
    ha_entity_key_t key;
    ha_entity_record_t record;
} fake_entity_t;

typedef struct {
    ha_state_key_t key;
    ha_state_record_t record;
} fake_state_t;

typedef struct {
    ha_group_key_t key;
    ha_group_record_t record;
} fake_group_t;

typedef struct {
    ha_group_item_key_t key;
    ha_group_item_record_t record;
} fake_item_t;

typedef struct {
    ha_entity_key_t key;
    ha_entity_capabilities_record_t record;
} fake_caps_t;

typedef struct {
    ha_device_id_t uid;
    ha_location_record_t record;
} fake_location_t;

static const fake_entity_t s_entities[] = {
    {{ENT_LIGHT}, {.device_id = HA_DEVICE_ID_NONE, .name = "Лампа RGB"}},
    {{ENT_SENSOR}, {.device_id = HA_DEVICE_ID_NONE, .name = "Датчик улица"}},
};

static const fake_state_t s_states[] = {
    {{ENT_LIGHT, HA_PROPERTY_POWER, 0}, {.value = {.kind = HA_VALUE_BOOL, .value = {.b = true}}}},
    {{ENT_LIGHT, HA_PROPERTY_BRIGHTNESS, 0},
     {.value = {.kind = HA_VALUE_FLOAT, .value = {.f32 = 75.0f}}}},
    {{ENT_LIGHT, HA_PROPERTY_COLOR_X, 0},
     {.value = {.kind = HA_VALUE_FLOAT, .value = {.f32 = 0.42f}}}},
    {{ENT_LIGHT, HA_PROPERTY_COLOR_Y, 0},
     {.value = {.kind = HA_VALUE_FLOAT, .value = {.f32 = 0.38f}}}},
    {{ENT_LIGHT, HA_PROPERTY_COLOR_TEMPERATURE, 0},
     {.value = {.kind = HA_VALUE_FLOAT, .value = {.f32 = 3200.0f}}}},
    {{ENT_SENSOR, HA_PROPERTY_TEMPERATURE, 0},
     {.value = {.kind = HA_VALUE_FLOAT, .value = {.f32 = 23.4f}}}},
    {{ENT_SENSOR, HA_PROPERTY_HUMIDITY, 0},
     {.value = {.kind = HA_VALUE_FLOAT, .value = {.f32 = 41.2f}}}},
    {{ENT_SENSOR, HA_PROPERTY_BATTERY_PERCENT, 0},
     {.value = {.kind = HA_VALUE_FLOAT, .value = {.f32 = 82.0f}}}},
    {{ENT_SYSTEM, HA_PROPERTY_SYSTEM_HOUR, 0},
     {.value = {.kind = HA_VALUE_U32, .value = {.u32 = 14}}}},
    {{ENT_SYSTEM, HA_PROPERTY_SYSTEM_MINUTE, 0},
     {.value = {.kind = HA_VALUE_U32, .value = {.u32 = 32}}}},
};

static const fake_location_t s_locations[] = {
    {HA_SYSTEM_DEVICE_ID, {.latitude = 55.75f, .longitude = 37.62f, .tz_offset_min = 180,
                           .name = "Москва"}},
};

static const ha_wifi_scan_record_t s_wifi_scan = {
    .count = 3,
    .aps = {
        {.ssid = "HomeNet", .rssi = -48, .auth = HA_WIFI_AUTH_WPA2},
        {.ssid = "Neighbor_5G", .rssi = -72, .auth = HA_WIFI_AUTH_WPA2},
        {.ssid = "CafeGuest", .rssi = -83, .auth = HA_WIFI_AUTH_OPEN},
    },
};

static const ha_wifi_status_record_t s_wifi_status = {
    .state = HA_WIFI_STATE_IDLE,
    .connected = 0,
    .ssid = "",
};

/* Настройки изменяемы: экран настроек пишет сюда через domain_entity_put. */
static ha_settings_record_t s_settings = {
    .screensaver_timeout_ms = 60000,
    .brightness_pct = 80,
};

static const ha_weather_record_t s_weather = {
    .condition = HA_WEATHER_PARTLYCLOUDY,
    .cloud_pct = 40,
    .temperature_c100 = 2150, /* 21.50 °C */
    .humidity_p100 = 4800,    /* 48.00 % */
    .pressure_hpa = 1013,
    .wind_kmh10 = 32,         /* 3.2 км/ч */
    .wind_dir_deg = 220,
};

static const fake_group_t s_groups[] = {
    {{1}, {.title = "Кухня"}},
    {{2}, {.title = "Климат"}},
};

static const fake_item_t s_items[] = {
    {{1, ENT_LIGHT}, {.order = 1, .title = "Люстра"}},
    {{2, ENT_SENSOR}, {.order = 1, .title = "Датчик"}},
};

static const fake_caps_t s_caps[] = {
    {{ENT_LIGHT},
     {.property_count = 4,
      .properties = {
          {.property = HA_PROPERTY_POWER,
           .action_count = 3,
           .actions = {HA_ACTION_ON, HA_ACTION_OFF, HA_ACTION_TOGGLE}},
          {.property = HA_PROPERTY_BRIGHTNESS, .action_count = 1, .actions = {HA_ACTION_SET}},
          {.property = HA_PROPERTY_COLOR_TEMPERATURE,
           .action_count = 1,
           .actions = {HA_ACTION_SET}},
          {.property = HA_PROPERTY_COLOR, .action_count = 1, .actions = {HA_ACTION_SET}},
      }}},
    {{ENT_SENSOR},
     {.property_count = 3,
      .properties = {
          {.property = HA_PROPERTY_TEMPERATURE, .action_count = 0},
          {.property = HA_PROPERTY_HUMIDITY, .action_count = 0},
          {.property = HA_PROPERTY_BATTERY_PERCENT, .action_count = 0},
      }}},
};

static sys_error_t not_found(void)
{
    return sys_error_make(SYS_LAYER_DOMAIN, SYS_CODE_NOT_FOUND);
}

void domain_fake_init(void)
{
}

sys_error_t domain_entity_get(domain_t *domain, domain_entity_t type, const void *key,
                              void *out_record)
{
    (void)domain;
    if (key == NULL || out_record == NULL) {
        return sys_error_make(SYS_LAYER_DOMAIN, SYS_CODE_INVALID_ARG);
    }

    if (type == (domain_entity_t)HA_ENTITY_ENTITY) {
        const ha_entity_id_t id = ((const ha_entity_key_t *)key)->id;
        for (size_t i = 0; i < sizeof(s_entities) / sizeof(s_entities[0]); ++i) {
            if (s_entities[i].key.id == id) {
                *(ha_entity_record_t *)out_record = s_entities[i].record;
                return SYS_OK;
            }
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_CAPABILITIES) {
        const ha_entity_id_t id = ((const ha_entity_key_t *)key)->id;
        for (size_t i = 0; i < sizeof(s_caps) / sizeof(s_caps[0]); ++i) {
            if (s_caps[i].key.id == id) {
                *(ha_entity_capabilities_record_t *)out_record = s_caps[i].record;
                return SYS_OK;
            }
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_PROPERTY_STATE) {
        for (size_t i = 0; i < sizeof(s_states) / sizeof(s_states[0]); ++i) {
            if (memcmp(&s_states[i].key, key, sizeof(ha_state_key_t)) == 0) {
                *(ha_state_record_t *)out_record = s_states[i].record;
                return SYS_OK;
            }
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_GROUP) {
        const ha_group_key_t group_key = *(const ha_group_key_t *)key;
        for (size_t i = 0; i < sizeof(s_groups) / sizeof(s_groups[0]); ++i) {
            if (s_groups[i].key.id == group_key.id) {
                *(ha_group_record_t *)out_record = s_groups[i].record;
                return SYS_OK;
            }
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_GROUP_ITEM) {
        for (size_t i = 0; i < sizeof(s_items) / sizeof(s_items[0]); ++i) {
            if (memcmp(&s_items[i].key, key, sizeof(ha_group_item_key_t)) == 0) {
                *(ha_group_item_record_t *)out_record = s_items[i].record;
                return SYS_OK;
            }
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_LOCATION) {
        const ha_device_id_t uid = *(const ha_device_id_t *)key;
        for (size_t i = 0; i < sizeof(s_locations) / sizeof(s_locations[0]); ++i) {
            if (s_locations[i].uid == uid) {
                *(ha_location_record_t *)out_record = s_locations[i].record;
                return SYS_OK;
            }
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_WEATHER) {
        if (*(const ha_device_id_t *)key == HA_WEATHER_DEVICE_ID) {
            *(ha_weather_record_t *)out_record = s_weather;
            return SYS_OK;
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_WIFI_SCAN) {
        if (*(const ha_device_id_t *)key == HA_WIFI_DEVICE_ID) {
            *(ha_wifi_scan_record_t *)out_record = s_wifi_scan;
            return SYS_OK;
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_WIFI_STATUS) {
        if (*(const ha_device_id_t *)key == HA_WIFI_DEVICE_ID) {
            *(ha_wifi_status_record_t *)out_record = s_wifi_status;
            return SYS_OK;
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_SETTINGS) {
        if (((const ha_settings_key_t *)key)->id == HA_SETTINGS_ID) {
            *(ha_settings_record_t *)out_record = s_settings;
            return SYS_OK;
        }
        return not_found();
    }
    return not_found();
}

sys_error_t domain_entity_put(domain_t *domain, domain_entity_t type, const void *key,
                              const void *record, const domain_fact_meta_t *meta,
                              bool *out_changed)
{
    (void)domain;
    (void)key;
    (void)meta;
    if (out_changed != NULL) {
        *out_changed = false;
    }
    if (type == (domain_entity_t)HA_ENTITY_SETTINGS && record != NULL) {
        s_settings = *(const ha_settings_record_t *)record;
        if (out_changed != NULL) {
            *out_changed = true;
        }
        return SYS_OK;
    }
    return SYS_OK;
}

sys_error_t domain_entity_iter(domain_t *domain, domain_entity_t type,
                               domain_entity_iter_cb_t callback, void *ctx)
{
    (void)domain;
    if (callback == NULL) {
        return sys_error_make(SYS_LAYER_DOMAIN, SYS_CODE_INVALID_ARG);
    }

    if (type == (domain_entity_t)HA_ENTITY_GROUP) {
        for (size_t i = 0; i < sizeof(s_groups) / sizeof(s_groups[0]); ++i) {
            if (!callback(&s_groups[i].key, &s_groups[i].record, ctx)) {
                break;
            }
        }
    } else if (type == (domain_entity_t)HA_ENTITY_GROUP_ITEM) {
        for (size_t i = 0; i < sizeof(s_items) / sizeof(s_items[0]); ++i) {
            if (!callback(&s_items[i].key, &s_items[i].record, ctx)) {
                break;
            }
        }
    }
    return SYS_OK;
}

sys_error_t domain_entity_meta(domain_t *domain, domain_entity_t type, const void *key,
                               domain_entity_meta_t *out_meta)
{
    (void)domain;
    (void)key;
    /* Скан статичен в превью — отдаём версию, чтобы список построился один раз. */
    if (type == (domain_entity_t)HA_ENTITY_WIFI_SCAN && out_meta != NULL) {
        out_meta->version.value = 1;
        return SYS_OK;
    }
    return not_found();
}

sys_error_t domain_post(domain_t *domain, domain_command_t type, const void *args, size_t args_size,
                        const domain_fact_target_t *target, const domain_fact_meta_t *meta)
{
    (void)domain;
    (void)type;
    (void)args;
    (void)args_size;
    (void)target;
    (void)meta;
    return SYS_OK;
}
