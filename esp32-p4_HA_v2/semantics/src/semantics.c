#include "semantics/semantics.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "ha_model/ha_zigbee.h"

/*
 * Приватная таблица нормализации (docs/PROPERTY_MODEL.md §4.2). Единственное место,
 * где пары (cluster, attr) получают смысл. `scale`/`offset` — лишь частный случай
 * стратегии `ZB_DECODE_SCALE`; нелинейные свойства задают собственный `decode_kind`.
 */

typedef enum {
    ZB_DECODE_IDENTITY = 0,     /* raw как есть */
    ZB_DECODE_SCALE,            /* raw * scale + offset */
    ZB_DECODE_ILLUMINANCE,      /* ZCL measured value → lux */
    ZB_DECODE_MIREDS_TO_KELVIN, /* 1e6 / mired */
    ZB_DECODE_BITMAP_BOOL,      /* bitmap → bool (младший бит) */
} zb_decode_kind_t;

typedef struct {
    uint16_t cluster_id;
    uint16_t attr_id;
    ha_property_id_t property;
    uint8_t expected_type; /* валидация/диагностика; не влияет на декодирование */
    uint8_t decode_kind;   /* zb_decode_kind_t */
    float scale;           /* только для ZB_DECODE_SCALE */
    float offset;
} zb_property_map_t;

static const zb_property_map_t ZB_MAP[] = {
    {HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, HA_PROPERTY_POWER, HA_ZB_TYPE_BOOL,
     ZB_DECODE_IDENTITY, 0.0f, 0.0f},
    {HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT, HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE,
     HA_PROPERTY_TEMPERATURE, HA_ZB_TYPE_INT16, ZB_DECODE_SCALE, 0.01f, 0.0f},
    {HA_ZB_CLUSTER_RELATIVE_HUMIDITY, HA_ZB_ATTR_HUMIDITY_MEASURED_VALUE, HA_PROPERTY_HUMIDITY,
     HA_ZB_TYPE_UINT16, ZB_DECODE_SCALE, 0.01f, 0.0f},
    {HA_ZB_CLUSTER_POWER_CONFIG, HA_ZB_ATTR_POWER_CONFIG_BATTERY_VOLTAGE,
     HA_PROPERTY_BATTERY_VOLTAGE, HA_ZB_TYPE_UINT8, ZB_DECODE_SCALE, 0.1f, 0.0f},
    {HA_ZB_CLUSTER_POWER_CONFIG, HA_ZB_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING,
     HA_PROPERTY_BATTERY_PERCENT, HA_ZB_TYPE_UINT8, ZB_DECODE_SCALE, 0.5f, 0.0f},
    {HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE,
     HA_PROPERTY_ILLUMINANCE, HA_ZB_TYPE_UINT16, ZB_DECODE_ILLUMINANCE, 0.0f, 0.0f},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE, HA_PROPERTY_COLOR_TEMPERATURE,
     HA_ZB_TYPE_UINT16, ZB_DECODE_MIREDS_TO_KELVIN, 0.0f, 0.0f},
    {HA_ZB_CLUSTER_OCCUPANCY_SENSING, HA_ZB_ATTR_OCCUPANCY_OCCUPANCY, HA_PROPERTY_OCCUPANCY,
     HA_ZB_TYPE_BITMAP8, ZB_DECODE_BITMAP_BOOL, 0.0f, 0.0f},
};

static const zb_property_map_t *map_find(uint16_t cluster_id, uint16_t attr_id)
{
    const size_t count = sizeof(ZB_MAP) / sizeof(ZB_MAP[0]);
    for (size_t i = 0; i < count; i++) {
        if (ZB_MAP[i].cluster_id == cluster_id && ZB_MAP[i].attr_id == attr_id) {
            return &ZB_MAP[i];
        }
    }
    return NULL;
}

/*
 * Значение по фактическому ZCL-типу (тот же разбор, что был в Automation): знак и
 * ширину задаёт тип записи, а не таблица. Тип вне словаря скаляров — отказ.
 */
static bool zcl_numeric(const ha_zb_state_record_t *state, double *out)
{
    const uint32_t raw = state->raw;
    switch (state->zcl_type) {
    case HA_ZB_TYPE_BOOL:
    case HA_ZB_TYPE_BITMAP8:
    case HA_ZB_TYPE_UINT8:
    case HA_ZB_TYPE_ENUM8:
        *out = (double)(raw & 0xffu);
        break;
    case HA_ZB_TYPE_INT8:
        *out = (double)(int8_t)(raw & 0xffu);
        break;
    case HA_ZB_TYPE_UINT16:
    case HA_ZB_TYPE_ENUM16:
        *out = (double)(raw & 0xffffu);
        break;
    case HA_ZB_TYPE_INT16:
        *out = (double)(int16_t)(raw & 0xffffu);
        break;
    case HA_ZB_TYPE_UINT32:
        *out = (double)raw;
        break;
    case HA_ZB_TYPE_INT32:
        *out = (double)(int32_t)raw;
        break;
    case HA_ZB_TYPE_SINGLE_FLOAT: {
        float f = 0.0f;
        memcpy(&f, &raw, sizeof(f));
        *out = (double)f;
        break;
    }
    default:
        return false;
    }
    return true;
}

static bool decode_kind(double raw, const zb_property_map_t *entry, double *out)
{
    switch ((zb_decode_kind_t)entry->decode_kind) {
    case ZB_DECODE_IDENTITY:
        *out = raw;
        return true;
    case ZB_DECODE_SCALE:
        *out = raw * (double)entry->scale + (double)entry->offset;
        return true;
    case ZB_DECODE_ILLUMINANCE:
        /* ZCL: 0 — слишком темно; 0xFFFF — invalid; иначе lux = 10^((m-1)/10000). */
        if (raw <= 0.0) {
            *out = 0.0;
            return true;
        }
        if (raw >= 65535.0) {
            return false;
        }
        *out = pow(10.0, (raw - 1.0) / 10000.0);
        return true;
    case ZB_DECODE_MIREDS_TO_KELVIN:
        if (raw <= 0.0) {
            return false;
        }
        *out = 1000000.0 / raw;
        return true;
    case ZB_DECODE_BITMAP_BOOL:
        *out = (((uint32_t)raw) & 1u) ? 1.0 : 0.0;
        return true;
    default:
        return false;
    }
}

static void store_value(ha_value_kind_t kind, double value, ha_value_t *out)
{
    out->kind = kind;
    switch (kind) {
    case HA_VALUE_BOOL:
        out->value.b = (value != 0.0);
        break;
    case HA_VALUE_I32:
        out->value.i32 = (int32_t)value;
        break;
    case HA_VALUE_U32:
        out->value.u32 = (uint32_t)value;
        break;
    case HA_VALUE_ENUM:
        out->value.enum_value = (uint32_t)value;
        break;
    case HA_VALUE_FLOAT:
        out->value.f32 = (float)value;
        break;
    default:
        out->kind = HA_VALUE_NONE;
        break;
    }
}

ha_property_id_t semantics_property_from_key(const ha_zb_state_key_t *key)
{
    if (key == NULL) {
        return HA_PROPERTY_UNKNOWN;
    }
    const zb_property_map_t *entry = map_find(key->cluster_id, key->attr_id);
    return (entry != NULL) ? entry->property : HA_PROPERTY_UNKNOWN;
}

bool semantics_state_value(const ha_zb_state_key_t *key, const ha_zb_state_record_t *state,
                           ha_value_t *out)
{
    if (key == NULL || state == NULL || out == NULL) {
        return false;
    }
    const zb_property_map_t *entry = map_find(key->cluster_id, key->attr_id);
    if (entry == NULL) {
        return false;
    }

    double raw = 0.0;
    if (!zcl_numeric(state, &raw)) {
        return false;
    }
    double value = 0.0;
    if (!decode_kind(raw, entry, &value)) {
        return false;
    }

    store_value(ha_property_desc(entry->property)->value_kind, value, out);
    return out->kind != HA_VALUE_NONE;
}
