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

/* --- Семантические события (фаза 4): physical → ha_event_t --- */

typedef struct {
    const char *model_match; /* подстрока модели устройства; NULL — общий профиль */
    uint16_t cluster_id;
    uint8_t command_id;
    ha_event_id_t event;
} zb_event_map_t;

static const zb_event_map_t ZB_EVENT_MAP[] = {
    /*
     * Кнопка/контроллер, приславший OnOff-команду координатору (лампа — сервер OnOff —
     * команд не шлёт). Для вендоров с иной кодировкой добавляются profile-записи с
     * `model_match`.
     */
    {NULL, HA_ZB_CLUSTER_ON_OFF, HA_ZB_CMD_ON_OFF_ON, HA_EVENT_SINGLE_PRESS},
    {NULL, HA_ZB_CLUSTER_ON_OFF, HA_ZB_CMD_ON_OFF_OFF, HA_EVENT_SINGLE_PRESS},
    {NULL, HA_ZB_CLUSTER_ON_OFF, HA_ZB_CMD_ON_OFF_TOGGLE, HA_EVENT_SINGLE_PRESS},
};

static bool model_matches(const char *model, const char *needle)
{
    if (needle == NULL) {
        return true; /* общий профиль */
    }
    return model != NULL && strstr(model, needle) != NULL;
}

bool semantics_decode_event(const ha_zb_event_t *physical, const ha_device_record_t *device,
                            ha_event_t *out)
{
    if (physical == NULL || out == NULL) {
        return false;
    }
    const char *model = (device != NULL) ? device->model : NULL;
    const size_t count = sizeof(ZB_EVENT_MAP) / sizeof(ZB_EVENT_MAP[0]);
    for (size_t i = 0; i < count; i++) {
        const zb_event_map_t *entry = &ZB_EVENT_MAP[i];
        if (entry->cluster_id == physical->cluster_id &&
            entry->command_id == physical->command_id && model_matches(model, entry->model_match)) {
            ha_event_t event = {0};
            event.id = entry->event;
            event.device_uid = physical->device_uid;
            event.endpoint = physical->endpoint;
            event.value.kind = HA_VALUE_NONE;
            *out = event;
            return true;
        }
    }
    return false;
}

/* --- Capabilities (фаза 5): что кластер/свойство умеет, без inference в UI --- */

size_t semantics_cluster_properties(uint16_t cluster_id, ha_property_id_t *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }
    size_t n = 0;
    const size_t count = sizeof(ZB_MAP) / sizeof(ZB_MAP[0]);
    for (size_t i = 0; i < count; i++) {
        if (ZB_MAP[i].cluster_id != cluster_id) {
            continue;
        }
        const ha_property_id_t property = ZB_MAP[i].property;
        bool duplicate = false;
        for (size_t j = 0; j < n; j++) {
            if (out[j] == property) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (n < max) {
            out[n] = property;
        }
        n++;
    }
    return (n < max) ? n : max;
}

size_t semantics_property_actions(ha_property_id_t property, ha_action_id_t *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }
    size_t n = 0;
    const size_t count = sizeof(ZB_ACTION_MAP) / sizeof(ZB_ACTION_MAP[0]);
    for (size_t i = 0; i < count; i++) {
        if (ZB_ACTION_MAP[i].property != property) {
            continue;
        }
        if (n < max) {
            out[n] = ZB_ACTION_MAP[i].action;
        }
        n++;
    }
    return (n < max) ? n : max;
}

/* --- Компилятор semantic-правила (фаза 5.3) ----------------------------- */

bool semantics_event_to_physical(const ha_device_record_t *device, ha_event_id_t event,
                                 uint16_t *out_command_id)
{
    if (out_command_id == NULL) {
        return false;
    }
    const char *model = (device != NULL) ? device->model : NULL;
    const size_t count = sizeof(ZB_EVENT_MAP) / sizeof(ZB_EVENT_MAP[0]);
    const zb_event_map_t *found = NULL;
    size_t matches = 0;
    for (size_t i = 0; i < count; i++) {
        if (ZB_EVENT_MAP[i].event == event && model_matches(model, ZB_EVENT_MAP[i].model_match)) {
            found = &ZB_EVENT_MAP[i];
            matches++;
        }
    }
    if (matches != 1) {
        return false; /* 0 — неизвестно, >1 — неоднозначно */
    }
    *out_command_id = found->command_id;
    return true;
}

bool semantics_compile_automation(const ha_sem_rule_t *rule, const ha_device_record_t *trigger_device,
                                  ha_automation_record_t *out)
{
    if (rule == NULL || out == NULL || rule->conditions_count > HA_AUTOMATION_CONDITIONS_MAX) {
        return false;
    }
    ha_automation_record_t record = {0};
    record.enabled = rule->enabled;
    record.trigger_kind = rule->trigger.kind;

    const ha_sem_trigger_t *trg = &rule->trigger;
    switch ((ha_automation_trigger_kind_t)trg->kind) {
    case HA_TRIGGER_DEVICE_EVENT: {
        record.trigger_b.event.device_uid = trg->device_uid;
        uint16_t command_id = 0;
        if (trg->device_uid == HA_SYSTEM_DEVICE_UID) {
            command_id = (uint16_t)trg->event_id; /* system-события адресуются event id */
        } else if (!semantics_event_to_physical(trigger_device, trg->event_id, &command_id)) {
            return false; /* невыразимо в legacy: не сохраняем неверное правило */
        }
        record.trigger_b.event.command_id = command_id;
        break;
    }
    case HA_TRIGGER_TIME:
        record.trigger_a.time.minutes_of_day = trg->minutes_of_day;
        record.trigger_a.time.weekday_mask = trg->weekday_mask;
        break;
    case HA_TRIGGER_STATE: {
        if ((ha_condition_op_t)trg->op == HA_CONDITION_OP_BETWEEN) {
            return false; /* у STATE-триггера нет второго порога в legacy-записи */
        }
        const ha_zb_state_key_t context = {.device_uid = trg->device_uid, .endpoint = trg->endpoint};
        ha_zb_state_key_t key = {0};
        if (!semantics_property_key(&context, trg->property, &key)) {
            return false;
        }
        record.trigger_b.state.device_uid = key.device_uid;
        record.trigger_b.state.endpoint = trg->endpoint; /* 0 — любой */
        record.trigger_b.state.cluster_id = key.cluster_id;
        record.trigger_b.state.attr_id = key.attr_id;
        record.trigger_b.state.op = trg->op;
        record.trigger_b.state.edge = trg->edge;
        record.trigger_a.state_value = trg->value;
        break;
    }
    default:
        return false;
    }

    record.conditions_count = rule->conditions_count;
    for (uint8_t i = 0; i < rule->conditions_count; i++) {
        const ha_sem_condition_t *c = &rule->conditions[i];
        const ha_zb_state_key_t context = {.device_uid = c->ref.device_uid, .endpoint = c->ref.endpoint};
        ha_zb_state_key_t key = {0};
        if (!semantics_property_key(&context, c->ref.property, &key)) {
            return false;
        }
        record.conditions[i].device_uid = c->ref.device_uid;
        record.conditions[i].endpoint = c->ref.endpoint;
        record.conditions[i].cluster_id = key.cluster_id;
        record.conditions[i].attr_id = key.attr_id;
        record.conditions[i].op = c->op;
        record.conditions[i].value = c->value;
        record.conditions[i].value2 = c->value2;
    }

    const ha_sem_action_t *act = &rule->action;
    ha_command_value_t value = {0};
    value.kind = (ha_command_value_kind_t)act->value_kind;
    if (value.kind == HA_COMMAND_VALUE_SCALAR) {
        value.value.scalar = act->value;
    } else if (value.kind == HA_COMMAND_VALUE_XY) {
        value.value.xy.x = act->x;
        value.value.xy.y = act->y;
    }
    const ha_zb_state_key_t target = {.device_uid = act->target.device_uid,
                                      .endpoint = act->target.endpoint};
    ha_zb_command_t command = {0};
    if (!semantics_build_command(&target, act->target.property, act->action, &value, &command)) {
        return false;
    }
    record.action_device_uid = command.device_uid;
    record.action_endpoint = command.dst_endpoint;
    record.action_cluster_id = command.cluster_id;
    record.action_command_id = command.command_id;
    record.action_args_len = command.args_len;
    memcpy(record.action_args, command.args, command.args_len);

    *out = record;
    return true;
}

/* --- Обратная проекция physical → semantic (фаза 5.3.4a) --- */

static bool physical_to_event(const ha_device_record_t *device, uint16_t command_id,
                              ha_event_id_t *out)
{
    const char *model = (device != NULL) ? device->model : NULL;
    const size_t count = sizeof(ZB_EVENT_MAP) / sizeof(ZB_EVENT_MAP[0]);
    const zb_event_map_t *found = NULL;
    size_t matches = 0;
    for (size_t i = 0; i < count; i++) {
        if (ZB_EVENT_MAP[i].command_id == command_id &&
            model_matches(model, ZB_EVENT_MAP[i].model_match)) {
            found = &ZB_EVENT_MAP[i];
            matches++;
        }
    }
    if (matches != 1) {
        return false;
    }
    *out = found->event;
    return true;
}

/* ZCL-команда → semantic action. false — нет пары, args не формы, ненулевой transition. */
static bool decompile_action(const ha_zb_command_t *cmd, ha_sem_action_t *out)
{
    const size_t count = sizeof(ZB_ACTION_MAP) / sizeof(ZB_ACTION_MAP[0]);
    const zb_action_map_t *entry = NULL;
    for (size_t i = 0; i < count; i++) {
        if (ZB_ACTION_MAP[i].cluster_id == cmd->cluster_id &&
            ZB_ACTION_MAP[i].command_id == cmd->command_id) {
            entry = &ZB_ACTION_MAP[i];
            break;
        }
    }
    if (entry == NULL) {
        return false;
    }
    const uint8_t *a = cmd->args;
    out->target.property = entry->property;
    out->action = entry->action;
    switch ((zb_cmd_encode_t)entry->encode) {
    case ZB_CMD_NONE:
        out->value_kind = HA_COMMAND_VALUE_NONE;
        return true;
    case ZB_CMD_LEVEL_PERCENT:
    case ZB_CMD_SAT_PERCENT:
        if (cmd->args_len != 3 || a[1] != 0 || a[2] != 0) {
            return false; /* transition != 0 — невыразимо */
        }
        out->value_kind = HA_COMMAND_VALUE_SCALAR;
        out->value.kind = HA_VALUE_FLOAT;
        out->value.value.f32 = (float)(a[0] * 100.0 / 254.0);
        return true;
    case ZB_CMD_HUE_DEG:
        if (cmd->args_len != 4 || a[1] != 0 || a[2] != 0 || a[3] != 0) {
            return false;
        }
        out->value_kind = HA_COMMAND_VALUE_SCALAR;
        out->value.kind = HA_VALUE_FLOAT;
        out->value.value.f32 = (float)(a[0] * 360.0 / 254.0);
        return true;
    case ZB_CMD_MIREDS_FROM_K: {
        if (cmd->args_len != 4 || a[2] != 0 || a[3] != 0) {
            return false;
        }
        const uint16_t mired = (uint16_t)(a[0] | (a[1] << 8));
        if (mired == 0) {
            return false;
        }
        out->value_kind = HA_COMMAND_VALUE_SCALAR;
        out->value.kind = HA_VALUE_FLOAT;
        out->value.value.f32 = (float)(1000000.0 / mired);
        return true;
    }
    case ZB_CMD_XY:
        if (cmd->args_len != 6 || a[4] != 0 || a[5] != 0) {
            return false;
        }
        out->value_kind = HA_COMMAND_VALUE_XY;
        out->x = (float)(uint16_t)(a[0] | (a[1] << 8)) / 65535.0f;
        out->y = (float)(uint16_t)(a[2] | (a[3] << 8)) / 65535.0f;
        return true;
    default:
        return false;
    }
}

bool semantics_decompile_automation(const ha_automation_record_t *record,
                                    const ha_device_record_t *trigger_device, ha_sem_rule_t *out)
{
    if (record == NULL || out == NULL || record->conditions_count > HA_AUTOMATION_CONDITIONS_MAX) {
        return false;
    }
    ha_sem_rule_t rule = {0};
    rule.enabled = record->enabled;
    rule.trigger.kind = record->trigger_kind;

    switch ((ha_automation_trigger_kind_t)record->trigger_kind) {
    case HA_TRIGGER_DEVICE_EVENT: {
        rule.trigger.device_uid = record->trigger_b.event.device_uid;
        if (record->trigger_b.event.device_uid == HA_SYSTEM_DEVICE_UID) {
            rule.trigger.event_id = (ha_event_id_t)record->trigger_b.event.command_id;
        } else if (!physical_to_event(trigger_device, record->trigger_b.event.command_id,
                                      &rule.trigger.event_id)) {
            return false; /* неоднозначно/неизвестно → legacy */
        }
        break;
    }
    case HA_TRIGGER_TIME:
        rule.trigger.minutes_of_day = record->trigger_a.time.minutes_of_day;
        rule.trigger.weekday_mask = record->trigger_a.time.weekday_mask;
        break;
    case HA_TRIGGER_STATE: {
        const ha_zb_state_key_t key = {.cluster_id = record->trigger_b.state.cluster_id,
                                       .attr_id = record->trigger_b.state.attr_id};
        rule.trigger.device_uid = record->trigger_b.state.device_uid;
        rule.trigger.endpoint = record->trigger_b.state.endpoint;
        rule.trigger.property = semantics_property_from_key(&key);
        if (rule.trigger.property == HA_PROPERTY_UNKNOWN) {
            return false;
        }
        rule.trigger.op = record->trigger_b.state.op;
        rule.trigger.edge = record->trigger_b.state.edge;
        rule.trigger.value = record->trigger_a.state_value;
        break;
    }
    default:
        return false;
    }

    rule.conditions_count = record->conditions_count;
    for (uint8_t i = 0; i < record->conditions_count; i++) {
        const ha_zb_state_key_t key = {.cluster_id = record->conditions[i].cluster_id,
                                       .attr_id = record->conditions[i].attr_id};
        ha_sem_condition_t *c = &rule.conditions[i];
        c->ref.property = semantics_property_from_key(&key);
        if (c->ref.property == HA_PROPERTY_UNKNOWN) {
            return false;
        }
        c->ref.device_uid = record->conditions[i].device_uid;
        c->ref.endpoint = record->conditions[i].endpoint;
        c->op = record->conditions[i].op;
        c->value = record->conditions[i].value;
        c->value2 = record->conditions[i].value2;
    }

    ha_zb_command_t cmd = {0};
    cmd.cluster_id = record->action_cluster_id;
    cmd.command_id = record->action_command_id;
    cmd.args_len = record->action_args_len;
    memcpy(cmd.args, record->action_args, record->action_args_len);
    if (!decompile_action(&cmd, &rule.action)) {
        return false;
    }
    rule.action.target.device_uid = record->action_device_uid;
    rule.action.target.endpoint = record->action_endpoint;

    *out = rule;
    return true;
}
