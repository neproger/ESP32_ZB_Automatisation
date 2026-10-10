#pragma once

/*
 * Семантический словарь (docs/PROPERTY_MODEL.md, фаза 0): transport-agnostic имена
 * того, что дом умеет измерять и чем управлять. Здесь НЕТ Zigbee: ни cluster_id,
 * ни attr_id, ни zcl_type, ни scale/offset — их знает только модуль-мост (позже).
 *
 * Это пассивный словарь: enum'ы, формы значения/дескриптора и чистый lookup.
 * Никакого поведения, состояния и линковки с Domain/Zigbee.
 *
 * Идентификаторы (`*_id_t`) — стабильный semantic ABI: заданы явными числами и
 * НИКОГДА не переиспользуются. Удалённое значение остаётся «дыркой», а не отдаётся
 * другому смыслу: позже эти числа попадут в API Automation/Web и в canonical key.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Что за величина у объекта. id — стабильный, не переиспользуется. */
typedef enum {
    HA_PROPERTY_UNKNOWN = 0,

    /* управляемые */
    HA_PROPERTY_POWER = 1,
    HA_PROPERTY_BRIGHTNESS = 2,
    HA_PROPERTY_COLOR_HUE = 3,
    HA_PROPERTY_COLOR_SATURATION = 4,
    HA_PROPERTY_COLOR_X = 5,
    HA_PROPERTY_COLOR_Y = 6,
    HA_PROPERTY_COLOR_TEMPERATURE = 7,

    /* измеряемые */
    HA_PROPERTY_TEMPERATURE = 8,
    HA_PROPERTY_HUMIDITY = 9,
    HA_PROPERTY_ILLUMINANCE = 10,
    HA_PROPERTY_OCCUPANCY = 11,
    HA_PROPERTY_BATTERY_VOLTAGE = 12,
    HA_PROPERTY_BATTERY_PERCENT = 13,

    /* синтетический системный девайс (время) */
    HA_PROPERTY_SYSTEM_TIME_UTC = 14,
    HA_PROPERTY_SYSTEM_TIME_STATUS = 15,
    HA_PROPERTY_SYSTEM_HOUR = 16,
    HA_PROPERTY_SYSTEM_MINUTE = 17,
    HA_PROPERTY_SYSTEM_WEEKDAY = 18,
    HA_PROPERTY_SYSTEM_WEEKDAY_MASK = 19,
    HA_PROPERTY_SYSTEM_MINUTES_OF_DAY = 20,
    HA_PROPERTY_SYSTEM_TZ_OFFSET = 21,

    /*
     * Командная capability (не состояние): состояния цвета — COLOR_X/COLOR_Y,
     * а команда физически задаёт цвет как одну величину — COLOR SET(x, y).
     */
    HA_PROPERTY_COLOR = 22,
} ha_property_id_t;

/* Намерение над свойством. Смысл — «что сделать», адресат задаёт вызывающий. */
typedef enum {
    HA_ACTION_NONE = 0,
    HA_ACTION_ON = 1,
    HA_ACTION_OFF = 2,
    HA_ACTION_TOGGLE = 3,
    HA_ACTION_SET = 4,
} ha_action_id_t;

/*
 * Общий semantic event vocabulary всей системы (не только кнопки): сюда же — системные
 * события. Потребителю не нужно знать, кто источник (Zigbee/System). Стабильные числа.
 */
typedef enum {
    HA_EVENT_NONE = 0,
    HA_EVENT_SINGLE_PRESS = 1,
    HA_EVENT_DOUBLE_PRESS = 2,
    HA_EVENT_HOLD = 3,
    /* системные события (не Zigbee): тики времени и смена погоды */
    HA_EVENT_MINUTE_TICK = 4,
    HA_EVENT_HALF_HOUR_TICK = 5,
    HA_EVENT_HOUR_TICK = 6,
    HA_EVENT_DAY_TICK = 7,
    HA_EVENT_WEATHER_CHANGED = 8,
} ha_event_id_t;

/* Вид значения — дискриминатор union'а в ha_value_t. */
typedef enum {
    HA_VALUE_NONE = 0,
    HA_VALUE_BOOL = 1,
    HA_VALUE_I32 = 2,
    HA_VALUE_U32 = 3,
    HA_VALUE_FLOAT = 4,
    HA_VALUE_ENUM = 5,
} ha_value_kind_t;

/* Единица семантического значения. */
typedef enum {
    HA_UNIT_NONE = 0,
    HA_UNIT_CELSIUS = 1,
    HA_UNIT_KELVIN = 2,
    HA_UNIT_PERCENT = 3,
    HA_UNIT_LUX = 4,
    HA_UNIT_VOLT = 5,
    HA_UNIT_MINUTE = 6,
} ha_unit_t;

/*
 * Единое runtime-значение (один контракт результата для всех потребителей).
 * Это ТОЛЬКО значение: ни property, ни unit, ни диапазона внутри — контекст даёт
 * ha_property_desc_t. Единицы/масштаб сюда не входят (это дело моста/потребителя).
 */
typedef struct {
    ha_value_kind_t kind;
    union {
        bool b;
        int32_t i32;
        uint32_t u32;
        float f32;
        uint32_t enum_value;
    } value;
} ha_value_t;

/* Общие подсказки потребителям (не Zigbee и не хранение). */
#define HA_PROPERTY_FLAG_NONE 0u
#define HA_PROPERTY_FLAG_READ_ONLY (1u << 0) /* измерение, не управляемое */

/*
 * Значение семантической команды. Скаляр — для level/яркости/hue/sat/color-temp;
 * XY — для цвета (COLOR SET(x, y)). Не все команды сводятся к одному скаляру.
 */
typedef enum {
    HA_COMMAND_VALUE_NONE = 0,
    HA_COMMAND_VALUE_SCALAR = 1,
    HA_COMMAND_VALUE_XY = 2,
} ha_command_value_kind_t;

typedef struct {
    ha_command_value_kind_t kind;
    union {
        ha_value_t scalar;
        struct {
            float x;
            float y;
        } xy;
    } value;
} ha_command_value_t;

/*
 * Семантическое событие: источник (device/endpoint) + event id + опциональный payload.
 * Эфемерно (не state); потребитель не знает, был источник Zigbee или System.
 */
typedef struct {
    ha_event_id_t id;
    uint64_t device_uid;
    uint8_t endpoint;
    ha_value_t value; /* HA_VALUE_NONE, если payload нет */
} ha_event_t;

/*
 * Semantic capability: свойство и доступные над ним действия. Для BFF/UI — что умеет
 * endpoint, без inference по cluster. `action_count == 0` — только чтение.
 */
#define HA_CAPABILITY_ACTIONS_MAX 4
typedef struct {
    ha_property_id_t property;
    uint8_t action_count;
    ha_action_id_t actions[HA_CAPABILITY_ACTIONS_MAX];
} ha_capability_t;

/* Описание свойства: смысл, а не кодировка. Диапазон — семантический. */
typedef struct {
    ha_property_id_t id;
    ha_value_kind_t value_kind;
    ha_unit_t unit;
    float range_min;
    float range_max;
    uint8_t flags; /* HA_PROPERTY_FLAG_* */
} ha_property_desc_t;

/*
 * Чистый lookup. Всегда возвращает валидный указатель: неизвестный/невалидный id даёт
 * дескриптор HA_PROPERTY_UNKNOWN (kind NONE). Честная деградация без ложной семантики.
 */
const ha_property_desc_t *ha_property_desc(ha_property_id_t id);

#ifdef __cplusplus
}
#endif
