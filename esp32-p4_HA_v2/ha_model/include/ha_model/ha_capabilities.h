#pragma once

/*
 * Проекция возможностей: (cluster_id, role) → capability
 * (docs/services/ZIGBEE_CAPABILITIES.md).
 *
 * Это словарь, а не логика: только числа и строки. Domain его не включает.
 * Потребитель (Web / Automation) ищет пару (cluster_id, role) среди кластеров
 * endpoint'а и берёт capability + стабильное имя. Решение «что это за endpoint» —
 * данные, а не поведение; вторая система идентификаторов не появляется.
 *
 * Наружу (в DTO) выходит строка `name`; enum — внутренний дискриминатор.
 */

#include <stdint.h>

#include "ha_model/ha_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HA_CAPABILITY_NONE = 0,
    HA_CAPABILITY_SWITCH,
    HA_CAPABILITY_DIMMABLE,
    HA_CAPABILITY_COLOR,
    HA_CAPABILITY_COVER,
    HA_CAPABILITY_THERMOSTAT,
    HA_CAPABILITY_FAN,
    HA_CAPABILITY_SENSOR_ILLUMINANCE,
    HA_CAPABILITY_SENSOR_TEMPERATURE,
    HA_CAPABILITY_SENSOR_HUMIDITY,
    HA_CAPABILITY_SENSOR_OCCUPANCY,
    HA_CAPABILITY_ALARM_ZONE,
    HA_CAPABILITY_METERING,
    HA_CAPABILITY_CONTROLLER_BUTTON,
    HA_CAPABILITY_CONTROLLER_DIMMER,
    HA_CAPABILITY_CONTROLLER_SCENE,
    HA_CAPABILITY_COUNT,
} ha_capability_t;

typedef struct {
    uint16_t cluster_id; /* HA_ZB_CLUSTER_* */
    uint8_t role;        /* HA_ZB_ROLE_* */
    uint8_t capability;  /* ha_capability_t */
    const char *name;    /* стабильное наружу имя capability */
} ha_capability_rule_t;

/*
 * Один capability может встречаться в нескольких правилах (Metering / Electrical
 * Measurement). Правило без совпадения = нет возможности.
 */
static const ha_capability_rule_t HA_CAPABILITY_RULES[] = {
    {HA_ZB_CLUSTER_ON_OFF, HA_ZB_ROLE_SERVER, HA_CAPABILITY_SWITCH, "switch"},
    {HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ROLE_SERVER, HA_CAPABILITY_DIMMABLE, "dimmable"},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ROLE_SERVER, HA_CAPABILITY_COLOR, "color"},
    {HA_ZB_CLUSTER_SHADE_CONFIG, HA_ZB_ROLE_SERVER, HA_CAPABILITY_COVER, "cover"},
    {HA_ZB_CLUSTER_WINDOW_COVERING, HA_ZB_ROLE_SERVER, HA_CAPABILITY_COVER, "cover"},
    {HA_ZB_CLUSTER_THERMOSTAT, HA_ZB_ROLE_SERVER, HA_CAPABILITY_THERMOSTAT, "thermostat"},
    {HA_ZB_CLUSTER_FAN_CONTROL, HA_ZB_ROLE_SERVER, HA_CAPABILITY_FAN, "fan"},
    {HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, HA_ZB_ROLE_SERVER,
     HA_CAPABILITY_SENSOR_ILLUMINANCE, "sensor.illuminance"},
    {HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT, HA_ZB_ROLE_SERVER,
     HA_CAPABILITY_SENSOR_TEMPERATURE, "sensor.temperature"},
    {HA_ZB_CLUSTER_RELATIVE_HUMIDITY, HA_ZB_ROLE_SERVER, HA_CAPABILITY_SENSOR_HUMIDITY,
     "sensor.humidity"},
    {HA_ZB_CLUSTER_OCCUPANCY_SENSING, HA_ZB_ROLE_SERVER, HA_CAPABILITY_SENSOR_OCCUPANCY,
     "sensor.occupancy"},
    {HA_ZB_CLUSTER_IAS_ZONE, HA_ZB_ROLE_SERVER, HA_CAPABILITY_ALARM_ZONE, "alarm.zone"},
    {HA_ZB_CLUSTER_METERING, HA_ZB_ROLE_SERVER, HA_CAPABILITY_METERING, "metering"},
    {HA_ZB_CLUSTER_ELECTRICAL_MEASUREMENT, HA_ZB_ROLE_SERVER, HA_CAPABILITY_METERING,
     "metering"},
    {HA_ZB_CLUSTER_ON_OFF, HA_ZB_ROLE_CLIENT, HA_CAPABILITY_CONTROLLER_BUTTON,
     "controller.button"},
    {HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ROLE_CLIENT, HA_CAPABILITY_CONTROLLER_DIMMER,
     "controller.dimmer"},
    {HA_ZB_CLUSTER_SCENES, HA_ZB_ROLE_CLIENT, HA_CAPABILITY_CONTROLLER_SCENE,
     "controller.scene"},
};

#define HA_CAPABILITY_RULES_COUNT \
    (sizeof(HA_CAPABILITY_RULES) / sizeof(HA_CAPABILITY_RULES[0]))

#ifdef __cplusplus
}
#endif
