/*
 * Фейковый Domain для host-превью (LVGL Live Preview): тот же UI-код, но данные —
 * мок. Реализованы только функции Domain, которые зовёт ui/. На прошивке линкуется
 * настоящий Domain (docs/clients/DISPLAY.md).
 */

#include <stdbool.h>
#include <string.h>

#include "domain/domain.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_settings.h"
#include "ha_model/ha_system.h"
#include "ha_model/ha_weather.h"
#include "ha_model/ha_wifi.h"
#include "ha_model/ha_zigbee.h"

#define DEV_KITCHEN 0x00124B0000000001ull
#define DEV_BEDROOM 0x00124B0000000002ull
#define DEV_SENSOR  0x00124B0000000003ull
#define DEV_LIGHT   0x00124B0000000004ull

typedef struct {
    ha_device_uid_t uid;
    ha_device_record_t record;
} fake_device_t;

typedef struct {
    ha_zb_state_key_t key;
    ha_zb_state_record_t record;
} fake_state_t;

typedef struct {
    ha_group_key_t key;
    ha_group_record_t record;
} fake_group_t;

typedef struct {
    ha_group_item_key_t key;
    ha_group_item_record_t record;
} fake_item_t;

static const fake_device_t s_devices[] = {
    {DEV_KITCHEN, {.name = "Кухня люстра"}},
    {DEV_BEDROOM, {.name = "Спальня свет"}},
    {DEV_SENSOR, {.name = "Датчик улица"}},
    {DEV_LIGHT, {.name = "Лампа RGB"}},
    {HA_SYSTEM_DEVICE_UID, {.name = "Время"}},
};

typedef struct {
    ha_device_uid_t uid;
    ha_location_record_t record;
} fake_location_t;

static const fake_location_t s_locations[] = {
    {HA_SYSTEM_DEVICE_UID, {.latitude = 55.75f, .longitude = 37.62f, .tz_offset_min = 180,
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

static const fake_state_t s_states[] = {
    /* Кухня */
    {{DEV_KITCHEN, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, 1, {0}},
     {.raw = 1, .zcl_type = HA_ZB_TYPE_BOOL}},
    {{DEV_KITCHEN, HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, 1, {0}},
     {.raw = 190, .zcl_type = HA_ZB_TYPE_UINT8}},
    /* Цветная лампа */
    {{DEV_LIGHT, HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_HUE, 1, {0}},
     {.raw = 120, .zcl_type = HA_ZB_TYPE_UINT8}},
    {{DEV_LIGHT, HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_SATURATION, 1, {0}},
     {.raw = 200, .zcl_type = HA_ZB_TYPE_UINT8}},
    {{DEV_LIGHT, HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE, 1, {0}},
     {.raw = 350, .zcl_type = HA_ZB_TYPE_UINT16}},
    /* Датчик */
    {{DEV_SENSOR, HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
      HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE, 1, {0}},
     {.raw = 2340, .zcl_type = HA_ZB_TYPE_INT16}},
    {{DEV_SENSOR, HA_ZB_CLUSTER_RELATIVE_HUMIDITY, HA_ZB_ATTR_HUMIDITY_MEASURED_VALUE, 1, {0}},
     {.raw = 4120, .zcl_type = HA_ZB_TYPE_UINT16}},
    {{DEV_SENSOR, HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT,
      HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE, 1, {0}},
     {.raw = 320, .zcl_type = HA_ZB_TYPE_UINT16}},
    {{DEV_SENSOR, HA_ZB_CLUSTER_POWER_CONFIG,
      HA_ZB_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING, 1, {0}},
     {.raw = 164, .zcl_type = HA_ZB_TYPE_UINT8}},
    {{DEV_SENSOR, HA_ZB_CLUSTER_POWER_CONFIG, HA_ZB_ATTR_POWER_CONFIG_BATTERY_VOLTAGE, 1, {0}},
     {.raw = 30, .zcl_type = HA_ZB_TYPE_UINT8}},
    {{DEV_SENSOR, HA_ZB_CLUSTER_OCCUPANCY_SENSING, HA_ZB_ATTR_OCCUPANCY_OCCUPANCY, 1, {0}},
     {.raw = 1, .zcl_type = HA_ZB_TYPE_BITMAP8}},
    {{DEV_SENSOR, HA_ZB_CLUSTER_IAS_ZONE, HA_ZB_ATTR_IAS_ZONE_ZONE_STATE, 1, {0}},
     {.raw = 0, .zcl_type = HA_ZB_TYPE_ENUM8}},
    {{DEV_SENSOR, HA_ZB_CLUSTER_METERING, 0x0000u, 1, {0}},
     {.raw = 12345, .zcl_type = HA_ZB_TYPE_UINT32}},
    /* Системное устройство: время/локация для строки состояния */
    {{HA_SYSTEM_DEVICE_UID, HA_CLUSTER_SYSTEM, HA_SYS_ATTR_HOUR, HA_SYSTEM_ENDPOINT, {0}},
     {.raw = 14, .zcl_type = HA_ZB_TYPE_UINT8}},
    {{HA_SYSTEM_DEVICE_UID, HA_CLUSTER_SYSTEM, HA_SYS_ATTR_MINUTE, HA_SYSTEM_ENDPOINT, {0}},
     {.raw = 32, .zcl_type = HA_ZB_TYPE_UINT8}},
    {{HA_SYSTEM_DEVICE_UID, HA_CLUSTER_SYSTEM, HA_SYS_ATTR_WEEKDAY, HA_SYSTEM_ENDPOINT, {0}},
     {.raw = 0, .zcl_type = HA_ZB_TYPE_UINT8}},
};

static const fake_group_t s_groups[] = {
    {{1}, {.title = "Кухня"}},
    {{2}, {.title = "Климат"}},
    {{3}, {.title = "Демо"}},
};

static const fake_item_t s_items[] = {
    /* Кухня */
    {{1, {DEV_KITCHEN, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, 1, {0}}},
     {.order = 1, .title = "Люстра"}},
    {{1, {DEV_KITCHEN, HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, 1, {0}}},
     {.order = 2, .title = "Яркость"}},
    /* Климат */
    {{2, {DEV_SENSOR, HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
          HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE, 1, {0}}},
     {.order = 1, .title = "Температура"}},
    {{2, {DEV_SENSOR, HA_ZB_CLUSTER_RELATIVE_HUMIDITY,
          HA_ZB_ATTR_HUMIDITY_MEASURED_VALUE, 1, {0}}},
     {.order = 2, .title = "Влажность"}},
    {{2, {DEV_SENSOR, HA_ZB_CLUSTER_POWER_CONFIG,
          HA_ZB_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING, 1, {0}}},
     {.order = 3, .title = "Батарея"}},
    /* Демо: по одному виджету каждого типа (скролл) */
    {{3, {DEV_KITCHEN, HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, 1, {0}}},
     {.order = 1, .title = "OnOff"}},
    {{3, {DEV_KITCHEN, HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, 1, {0}}},
     {.order = 2, .title = "Level"}},
    {{3, {DEV_LIGHT, HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_HUE, 1, {0}}},
     {.order = 3, .title = "Цвет"}},
    {{3, {DEV_LIGHT, HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE, 1, {0}}},
     {.order = 5, .title = "Цвет. темп."}},
    {{3, {DEV_SENSOR, HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
          HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE, 1, {0}}},
     {.order = 6, .title = "Температура"}},
    {{3, {DEV_SENSOR, HA_ZB_CLUSTER_RELATIVE_HUMIDITY,
          HA_ZB_ATTR_HUMIDITY_MEASURED_VALUE, 1, {0}}},
     {.order = 7, .title = "Влажность"}},
    {{3, {DEV_SENSOR, HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT,
          HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE, 1, {0}}},
     {.order = 8, .title = "Освещённость"}},
    {{3, {DEV_SENSOR, HA_ZB_CLUSTER_POWER_CONFIG,
          HA_ZB_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING, 1, {0}}},
     {.order = 9, .title = "Батарея (%)"}},
    {{3, {DEV_SENSOR, HA_ZB_CLUSTER_POWER_CONFIG, HA_ZB_ATTR_POWER_CONFIG_BATTERY_VOLTAGE, 1, {0}}},
     {.order = 10, .title = "Напряжение"}},
    {{3, {DEV_SENSOR, HA_ZB_CLUSTER_OCCUPANCY_SENSING, HA_ZB_ATTR_OCCUPANCY_OCCUPANCY, 1, {0}}},
     {.order = 11, .title = "Присутствие"}},
    {{3, {DEV_SENSOR, HA_ZB_CLUSTER_IAS_ZONE, HA_ZB_ATTR_IAS_ZONE_ZONE_STATE, 1, {0}}},
     {.order = 12, .title = "Охрана"}},
    {{3, {DEV_SENSOR, HA_ZB_CLUSTER_METERING, 0x0000u, 1, {0}}},
     {.order = 13, .title = "Счётчик"}},
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

    if (type == (domain_entity_t)HA_ENTITY_DEVICE) {
        const ha_device_uid_t uid = *(const ha_device_uid_t *)key;
        for (size_t i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); ++i) {
            if (s_devices[i].uid == uid) {
                *(ha_device_record_t *)out_record = s_devices[i].record;
                return SYS_OK;
            }
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_STATE) {
        for (size_t i = 0; i < sizeof(s_states) / sizeof(s_states[0]); ++i) {
            if (memcmp(&s_states[i].key, key, sizeof(ha_zb_state_key_t)) == 0) {
                *(ha_zb_state_record_t *)out_record = s_states[i].record;
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
        const ha_device_uid_t uid = *(const ha_device_uid_t *)key;
        for (size_t i = 0; i < sizeof(s_locations) / sizeof(s_locations[0]); ++i) {
            if (s_locations[i].uid == uid) {
                *(ha_location_record_t *)out_record = s_locations[i].record;
                return SYS_OK;
            }
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_WEATHER) {
        if (*(const ha_device_uid_t *)key == HA_WEATHER_DEVICE_UID) {
            *(ha_weather_record_t *)out_record = s_weather;
            return SYS_OK;
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_WIFI_SCAN) {
        if (*(const ha_device_uid_t *)key == HA_WIFI_DEVICE_UID) {
            *(ha_wifi_scan_record_t *)out_record = s_wifi_scan;
            return SYS_OK;
        }
        return not_found();
    }
    if (type == (domain_entity_t)HA_ENTITY_WIFI_STATUS) {
        if (*(const ha_device_uid_t *)key == HA_WIFI_DEVICE_UID) {
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
