#include "semantics/semantics.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "ha_model/ha_system.h"
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
    /* level 0..254 → процент */
    {HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, HA_PROPERTY_BRIGHTNESS,
     HA_ZB_TYPE_UINT8, ZB_DECODE_SCALE, 100.0f / 254.0f, 0.0f},
    /* ZCL hue 0..254 → градусы 0..360 */
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_HUE, HA_PROPERTY_COLOR_HUE,
     HA_ZB_TYPE_UINT8, ZB_DECODE_SCALE, 360.0f / 254.0f, 0.0f},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_SATURATION, HA_PROPERTY_COLOR_SATURATION,
     HA_ZB_TYPE_UINT8, ZB_DECODE_SCALE, 100.0f / 254.0f, 0.0f},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_X, HA_PROPERTY_COLOR_X,
     HA_ZB_TYPE_UINT16, ZB_DECODE_SCALE, 1.0f / 65535.0f, 0.0f},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_Y, HA_PROPERTY_COLOR_Y,
     HA_ZB_TYPE_UINT16, ZB_DECODE_SCALE, 1.0f / 65535.0f, 0.0f},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE, HA_PROPERTY_COLOR_TEMPERATURE,
     HA_ZB_TYPE_UINT16, ZB_DECODE_MIREDS_TO_KELVIN, 0.0f, 0.0f},
    {HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT, HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE,
     HA_PROPERTY_TEMPERATURE, HA_ZB_TYPE_INT16, ZB_DECODE_SCALE, 0.01f, 0.0f},
    {HA_ZB_CLUSTER_RELATIVE_HUMIDITY, HA_ZB_ATTR_HUMIDITY_MEASURED_VALUE, HA_PROPERTY_HUMIDITY,
     HA_ZB_TYPE_UINT16, ZB_DECODE_SCALE, 0.01f, 0.0f},
    {HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE,
     HA_PROPERTY_ILLUMINANCE, HA_ZB_TYPE_UINT16, ZB_DECODE_ILLUMINANCE, 0.0f, 0.0f},
    {HA_ZB_CLUSTER_OCCUPANCY_SENSING, HA_ZB_ATTR_OCCUPANCY_OCCUPANCY, HA_PROPERTY_OCCUPANCY,
     HA_ZB_TYPE_BITMAP8, ZB_DECODE_BITMAP_BOOL, 0.0f, 0.0f},
    {HA_ZB_CLUSTER_POWER_CONFIG, HA_ZB_ATTR_POWER_CONFIG_BATTERY_VOLTAGE,
     HA_PROPERTY_BATTERY_VOLTAGE, HA_ZB_TYPE_UINT8, ZB_DECODE_SCALE, 0.1f, 0.0f},
    {HA_ZB_CLUSTER_POWER_CONFIG, HA_ZB_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING,
     HA_PROPERTY_BATTERY_PERCENT, HA_ZB_TYPE_UINT8, ZB_DECODE_SCALE, 0.5f, 0.0f},
    /* системный девайс (время): значения как есть */
    {HA_CLUSTER_TIME, HA_TIME_ATTR_UTC_TIME, HA_PROPERTY_SYSTEM_TIME_UTC, HA_ZB_TYPE_UINT32,
     ZB_DECODE_IDENTITY, 0.0f, 0.0f},
    {HA_CLUSTER_TIME, HA_TIME_ATTR_TIME_STATUS, HA_PROPERTY_SYSTEM_TIME_STATUS, HA_ZB_TYPE_BITMAP8,
     ZB_DECODE_IDENTITY, 0.0f, 0.0f},
    {HA_CLUSTER_SYSTEM, HA_SYS_ATTR_HOUR, HA_PROPERTY_SYSTEM_HOUR, HA_ZB_TYPE_UINT8,
     ZB_DECODE_IDENTITY, 0.0f, 0.0f},
    {HA_CLUSTER_SYSTEM, HA_SYS_ATTR_MINUTE, HA_PROPERTY_SYSTEM_MINUTE, HA_ZB_TYPE_UINT8,
     ZB_DECODE_IDENTITY, 0.0f, 0.0f},
    {HA_CLUSTER_SYSTEM, HA_SYS_ATTR_WEEKDAY, HA_PROPERTY_SYSTEM_WEEKDAY, HA_ZB_TYPE_UINT8,
     ZB_DECODE_IDENTITY, 0.0f, 0.0f},
    {HA_CLUSTER_SYSTEM, HA_SYS_ATTR_WEEKDAY_MASK, HA_PROPERTY_SYSTEM_WEEKDAY_MASK,
     HA_ZB_TYPE_BITMAP8, ZB_DECODE_IDENTITY, 0.0f, 0.0f},
    {HA_CLUSTER_SYSTEM, HA_SYS_ATTR_MINUTES_OF_DAY, HA_PROPERTY_SYSTEM_MINUTES_OF_DAY,
     HA_ZB_TYPE_UINT16, ZB_DECODE_IDENTITY, 0.0f, 0.0f},
    {HA_CLUSTER_SYSTEM, HA_SYS_ATTR_TZ_OFFSET_MIN, HA_PROPERTY_SYSTEM_TZ_OFFSET, HA_ZB_TYPE_INT16,
     ZB_DECODE_IDENTITY, 0.0f, 0.0f},
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

static const zb_property_map_t *map_find_by_property(ha_property_id_t property)
{
    const size_t count = sizeof(ZB_MAP) / sizeof(ZB_MAP[0]);
    for (size_t i = 0; i < count; i++) {
        if (ZB_MAP[i].property == property) {
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

bool semantics_property_key(const ha_zb_state_key_t *context, ha_property_id_t property,
                            ha_zb_state_key_t *out)
{
    if (context == NULL || out == NULL) {
        return false;
    }
    const zb_property_map_t *entry = map_find_by_property(property);
    if (entry == NULL) {
        return false;
    }
    ha_zb_state_key_t key = {0};
    key.device_uid = context->device_uid;
    key.endpoint = context->endpoint;
    key.cluster_id = entry->cluster_id;
    key.attr_id = entry->attr_id;
    *out = key;
    return true;
}

bool semantics_value_to_double(const ha_value_t *value, double *out)
{
    if (value == NULL || out == NULL) {
        return false;
    }
    switch (value->kind) {
    case HA_VALUE_BOOL:
        *out = value->value.b ? 1.0 : 0.0;
        return true;
    case HA_VALUE_I32:
        *out = (double)value->value.i32;
        return true;
    case HA_VALUE_U32:
        *out = (double)value->value.u32;
        return true;
    case HA_VALUE_FLOAT:
        *out = (double)value->value.f32;
        return true;
    case HA_VALUE_ENUM:
        *out = (double)value->value.enum_value;
        return true;
    default:
        return false;
    }
}

/* --- Семантические команды (фаза 3): (property, action, value) → ZCL --- */

typedef enum {
    ZB_CMD_NONE = 0,      /* без аргументов */
    ZB_CMD_LEVEL_PERCENT, /* SET: % → 0..254 */
    ZB_CMD_HUE_DEG,       /* SET: градусы → 0..254 (direction shortest) */
    ZB_CMD_SAT_PERCENT,   /* SET: % → 0..254 */
    ZB_CMD_MIREDS_FROM_K, /* SET: K → mireds */
    ZB_CMD_XY,            /* SET: xy 0..1 → 0..65535 */
} zb_cmd_encode_t;

typedef struct {
    ha_property_id_t property;
    ha_action_id_t action;
    uint16_t cluster_id;
    uint8_t command_id;
    uint8_t encode; /* zb_cmd_encode_t */
} zb_action_map_t;

static const zb_action_map_t ZB_ACTION_MAP[] = {
    {HA_PROPERTY_POWER, HA_ACTION_ON, HA_ZB_CLUSTER_ON_OFF, HA_ZB_CMD_ON_OFF_ON, ZB_CMD_NONE},
    {HA_PROPERTY_POWER, HA_ACTION_OFF, HA_ZB_CLUSTER_ON_OFF, HA_ZB_CMD_ON_OFF_OFF, ZB_CMD_NONE},
    {HA_PROPERTY_POWER, HA_ACTION_TOGGLE, HA_ZB_CLUSTER_ON_OFF, HA_ZB_CMD_ON_OFF_TOGGLE, ZB_CMD_NONE},
    {HA_PROPERTY_BRIGHTNESS, HA_ACTION_SET, HA_ZB_CLUSTER_LEVEL_CONTROL,
     HA_ZB_CMD_LEVEL_MOVE_TO_LEVEL, ZB_CMD_LEVEL_PERCENT},
    {HA_PROPERTY_COLOR_HUE, HA_ACTION_SET, HA_ZB_CLUSTER_COLOR_CONTROL,
     HA_ZB_CMD_COLOR_MOVE_TO_HUE, ZB_CMD_HUE_DEG},
    {HA_PROPERTY_COLOR_SATURATION, HA_ACTION_SET, HA_ZB_CLUSTER_COLOR_CONTROL,
     HA_ZB_CMD_COLOR_MOVE_TO_SATURATION, ZB_CMD_SAT_PERCENT},
    {HA_PROPERTY_COLOR_TEMPERATURE, HA_ACTION_SET, HA_ZB_CLUSTER_COLOR_CONTROL,
     HA_ZB_CMD_COLOR_MOVE_TO_COLOR_TEMPERATURE, ZB_CMD_MIREDS_FROM_K},
    {HA_PROPERTY_COLOR, HA_ACTION_SET, HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_CMD_COLOR_MOVE_TO_COLOR,
     ZB_CMD_XY},
};

static const zb_action_map_t *action_find(ha_property_id_t property, ha_action_id_t action)
{
    const size_t count = sizeof(ZB_ACTION_MAP) / sizeof(ZB_ACTION_MAP[0]);
    for (size_t i = 0; i < count; i++) {
        if (ZB_ACTION_MAP[i].property == property && ZB_ACTION_MAP[i].action == action) {
            return &ZB_ACTION_MAP[i];
        }
    }
    return NULL;
}

static bool command_scalar(const ha_command_value_t *value, double *out)
{
    if (value == NULL || value->kind != HA_COMMAND_VALUE_SCALAR) {
        return false;
    }
    return semantics_value_to_double(&value->value.scalar, out);
}

static uint8_t percent_to_level(double pct)
{
    if (pct < 0.0) {
        pct = 0.0;
    }
    if (pct > 100.0) {
        pct = 100.0;
    }
    return (uint8_t)(pct * 254.0 / 100.0 + 0.5);
}

static uint8_t degrees_to_hue(double deg)
{
    deg = fmod(deg, 360.0);
    if (deg < 0.0) {
        deg += 360.0;
    }
    return (uint8_t)(deg * 254.0 / 360.0 + 0.5);
}

static uint16_t kelvin_to_mireds(double kelvin)
{
    if (kelvin <= 0.0) {
        return 0;
    }
    double m = 1000000.0 / kelvin;
    if (m > 65535.0) {
        m = 65535.0;
    }
    return (uint16_t)(m + 0.5);
}

static uint16_t unit_to_u16(double v)
{
    if (v < 0.0) {
        v = 0.0;
    }
    if (v > 1.0) {
        v = 1.0;
    }
    return (uint16_t)(v * 65535.0 + 0.5);
}

static bool encode_args(uint8_t encode, const ha_command_value_t *value, uint8_t *args, uint8_t *len)
{
    for (int i = 0; i < 6; i++) {
        args[i] = 0; /* transition time / direction / резерв — всегда в мосте */
    }
    switch ((zb_cmd_encode_t)encode) {
    case ZB_CMD_NONE:
        *len = 0;
        return true;
    case ZB_CMD_LEVEL_PERCENT: {
        double pct = 0.0;
        if (!command_scalar(value, &pct)) {
            return false;
        }
        args[0] = percent_to_level(pct);
        *len = 3;
        return true;
    }
    case ZB_CMD_HUE_DEG: {
        double deg = 0.0;
        if (!command_scalar(value, &deg)) {
            return false;
        }
        args[0] = degrees_to_hue(deg);
        args[1] = 0; /* direction: shortest */
        *len = 4;
        return true;
    }
    case ZB_CMD_SAT_PERCENT: {
        double pct = 0.0;
        if (!command_scalar(value, &pct)) {
            return false;
        }
        args[0] = percent_to_level(pct);
        *len = 3;
        return true;
    }
    case ZB_CMD_MIREDS_FROM_K: {
        double kelvin = 0.0;
        if (!command_scalar(value, &kelvin)) {
            return false;
        }
        const uint16_t mireds = kelvin_to_mireds(kelvin);
        args[0] = (uint8_t)(mireds & 0xFFu);
        args[1] = (uint8_t)(mireds >> 8);
        *len = 4;
        return true;
    }
    case ZB_CMD_XY: {
        if (value == NULL || value->kind != HA_COMMAND_VALUE_XY) {
            return false;
        }
        const uint16_t x = unit_to_u16(value->value.xy.x);
        const uint16_t y = unit_to_u16(value->value.xy.y);
        args[0] = (uint8_t)(x & 0xFFu);
        args[1] = (uint8_t)(x >> 8);
        args[2] = (uint8_t)(y & 0xFFu);
        args[3] = (uint8_t)(y >> 8);
        *len = 6;
        return true;
    }
    default:
        return false;
    }
}

bool semantics_build_command(const ha_zb_state_key_t *target, ha_property_id_t property,
                             ha_action_id_t action, const ha_command_value_t *value,
                             ha_zb_command_t *out)
{
    if (target == NULL || out == NULL) {
        return false;
    }
    const zb_action_map_t *entry = action_find(property, action);
    if (entry == NULL) {
        return false;
    }
    ha_zb_command_t command = {0};
    command.device_uid = target->device_uid;
    command.dst_endpoint = target->endpoint;
    command.cluster_id = entry->cluster_id;
    command.command_id = entry->command_id;
    if (!encode_args(entry->encode, value, command.args, &command.args_len)) {
        return false;
    }
    *out = command;
    return true;
}
