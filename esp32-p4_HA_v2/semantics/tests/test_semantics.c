#include "semantics/semantics.h"

#include <math.h>
#include <stdio.h>

#include "ha_model/ha_automation.h"
#include "ha_model/ha_properties.h"
#include "ha_model/ha_system.h"
#include "ha_model/ha_zigbee.h"

/*
 * Фаза 1 (docs/PROPERTY_MODEL.md): мост ZCL ↔ семантика.
 *   §1 — паритет с существующей математикой Automation (линейные, уже декодируемые);
 *   §2 — отдельно корректная нормализация нелинейных свойств.
 * Разделение намеренное: миграция архитектуры не смешивается с исправлением математики.
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                       \
    do {                                                                            \
        double _a = (a);                                                            \
        double _b = (b);                                                            \
        if (fabs(_a - _b) > (eps)) {                                                \
            printf("FAIL %s:%d: %s=%.6f != %.6f\n", __FILE__, __LINE__, #a, _a, _b); \
            g_failures++;                                                           \
        }                                                                           \
    } while (0)

static ha_zb_state_key_t key(uint16_t cluster, uint16_t attr)
{
    ha_zb_state_key_t k = {0};
    k.cluster_id = cluster;
    k.attr_id = attr;
    return k;
}

static ha_zb_state_record_t st(uint32_t raw, uint8_t type)
{
    ha_zb_state_record_t s = {0};
    s.raw = raw;
    s.zcl_type = type;
    return s;
}

static double val_double(const ha_value_t *v)
{
    switch (v->kind) {
    case HA_VALUE_BOOL:
        return v->value.b ? 1.0 : 0.0;
    case HA_VALUE_I32:
        return (double)v->value.i32;
    case HA_VALUE_U32:
        return (double)v->value.u32;
    case HA_VALUE_FLOAT:
        return (double)v->value.f32;
    case HA_VALUE_ENUM:
        return (double)v->value.enum_value;
    default:
        return 0.0;
    }
}

static ha_command_value_t scalar(float v)
{
    ha_command_value_t cv = {0};
    cv.kind = HA_COMMAND_VALUE_SCALAR;
    cv.value.scalar.kind = HA_VALUE_FLOAT;
    cv.value.scalar.value.f32 = v;
    return cv;
}

/* decode convenience: false if unmapped/undecodable. */
static bool decode(uint16_t cluster, uint16_t attr, uint32_t raw, uint8_t type, double *out)
{
    const ha_zb_state_key_t k = key(cluster, attr);
    const ha_zb_state_record_t s = st(raw, type);
    ha_value_t v = {0};
    if (!semantics_state_value(&k, &s, &v)) {
        return false;
    }
    *out = val_double(&v);
    return true;
}

/* --- §1 Паритет с существующим поведением Automation (линейное) --- */
static void test_parity(void)
{
    double v = 0.0;

    CHECK(decode(HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT, HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE,
                 2150, HA_ZB_TYPE_INT16, &v));
    CHECK_NEAR(v, 21.50, 1e-3);

    /* отрицательная температура: INT16 со знаком */
    CHECK(decode(HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT, HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE,
                 0xFFDEu, HA_ZB_TYPE_INT16, &v)); /* -34 */
    CHECK_NEAR(v, -0.34, 1e-3);

    CHECK(decode(HA_ZB_CLUSTER_RELATIVE_HUMIDITY, HA_ZB_ATTR_HUMIDITY_MEASURED_VALUE, 5500,
                 HA_ZB_TYPE_UINT16, &v));
    CHECK_NEAR(v, 55.00, 1e-3);

    CHECK(decode(HA_ZB_CLUSTER_POWER_CONFIG, HA_ZB_ATTR_POWER_CONFIG_BATTERY_VOLTAGE, 30,
                 HA_ZB_TYPE_UINT8, &v));
    CHECK_NEAR(v, 3.0, 1e-3);

    CHECK(decode(HA_ZB_CLUSTER_POWER_CONFIG, HA_ZB_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING,
                 200, HA_ZB_TYPE_UINT8, &v));
    CHECK_NEAR(v, 100.0, 1e-3);

    /* On/Off: bool как 0/1 */
    CHECK(decode(HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, 1, HA_ZB_TYPE_BOOL, &v));
    CHECK_NEAR(v, 1.0, 1e-6);
    CHECK(decode(HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, 0, HA_ZB_TYPE_BOOL, &v));
    CHECK_NEAR(v, 0.0, 1e-6);
}

/* --- §2 Нормализация нелинейных свойств (новая семантика, отдельно) --- */
static void test_normalization(void)
{
    double v = 0.0;

    /* illuminance: lux = 10^((m-1)/10000) */
    CHECK(decode(HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE, 1,
                 HA_ZB_TYPE_UINT16, &v));
    CHECK_NEAR(v, 1.0, 1e-3);
    CHECK(decode(HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE,
                 10001, HA_ZB_TYPE_UINT16, &v));
    CHECK_NEAR(v, 10.0, 1e-3);
    CHECK(decode(HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE,
                 20001, HA_ZB_TYPE_UINT16, &v));
    CHECK_NEAR(v, 100.0, 1e-3);
    CHECK(decode(HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE, 0,
                 HA_ZB_TYPE_UINT16, &v));
    CHECK_NEAR(v, 0.0, 1e-6);
    /* invalid */
    CHECK(!decode(HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE,
                  0xFFFFu, HA_ZB_TYPE_UINT16, &v));

    /* color temperature: mired → Kelvin */
    CHECK(decode(HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE, 500,
                 HA_ZB_TYPE_UINT16, &v));
    CHECK_NEAR(v, 2000.0, 1e-2);
    CHECK(decode(HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE, 333,
                 HA_ZB_TYPE_UINT16, &v));
    CHECK_NEAR(v, 3003.003, 1e-2);
    CHECK(!decode(HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE, 0,
                  HA_ZB_TYPE_UINT16, &v));

    /* occupancy: bitmap → bool */
    CHECK(decode(HA_ZB_CLUSTER_OCCUPANCY_SENSING, HA_ZB_ATTR_OCCUPANCY_OCCUPANCY, 1,
                 HA_ZB_TYPE_BITMAP8, &v));
    CHECK_NEAR(v, 1.0, 1e-6);
    CHECK(decode(HA_ZB_CLUSTER_OCCUPANCY_SENSING, HA_ZB_ATTR_OCCUPANCY_OCCUPANCY, 0,
                 HA_ZB_TYPE_BITMAP8, &v));
    CHECK_NEAR(v, 0.0, 1e-6);
    CHECK(decode(HA_ZB_CLUSTER_OCCUPANCY_SENSING, HA_ZB_ATTR_OCCUPANCY_OCCUPANCY, 2,
                 HA_ZB_TYPE_BITMAP8, &v)); /* bit0 не выставлен */
    CHECK_NEAR(v, 0.0, 1e-6);

    /* level 0..254 → процент */
    CHECK(decode(HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, 127,
                 HA_ZB_TYPE_UINT8, &v));
    CHECK_NEAR(v, 50.0, 0.2);
    /* ZCL hue 0..254 → градусы 0..360 */
    CHECK(decode(HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_HUE, 254,
                 HA_ZB_TYPE_UINT8, &v));
    CHECK_NEAR(v, 360.0, 1e-2);
    CHECK(decode(HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_SATURATION, 127,
                 HA_ZB_TYPE_UINT8, &v));
    CHECK_NEAR(v, 50.0, 0.2);
    /* xy 0..65535 → 0..1 */
    CHECK(decode(HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_X, 65535,
                 HA_ZB_TYPE_UINT16, &v));
    CHECK_NEAR(v, 1.0, 1e-4);
}

/* --- Обратный маппинг: property → физический ключ в том же объекте --- */
static void test_property_key(void)
{
    ha_zb_state_key_t context = {0};
    context.device_uid = 0x00124B000A1B2C3Dull;
    context.endpoint = 1;

    ha_zb_state_key_t out = {0};
    CHECK(semantics_property_key(&context, HA_PROPERTY_COLOR_X, &out));
    CHECK(out.device_uid == context.device_uid);
    CHECK(out.endpoint == 1);
    CHECK(out.cluster_id == HA_ZB_CLUSTER_COLOR_CONTROL);
    CHECK(out.attr_id == HA_ZB_ATTR_COLOR_CURRENT_X);

    CHECK(semantics_property_key(&context, HA_PROPERTY_BRIGHTNESS, &out));
    CHECK(out.cluster_id == HA_ZB_CLUSTER_LEVEL_CONTROL);
    CHECK(out.attr_id == HA_ZB_ATTR_LEVEL_CURRENT_LEVEL);

    CHECK(semantics_property_key(&context, HA_PROPERTY_POWER, &out));
    CHECK(out.cluster_id == HA_ZB_CLUSTER_ON_OFF);

    /* командное свойство (нет state-ключа) и NULL-контекст → false */
    CHECK(!semantics_property_key(&context, HA_PROPERTY_COLOR, &out));
    CHECK(!semantics_property_key(NULL, HA_PROPERTY_POWER, &out));
}

/* --- Семантическая идентификация и отказы --- */
static void test_identity_and_failures(void)
{
    const ha_zb_state_key_t temp = key(HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
                                       HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE);
    CHECK(semantics_property_from_key(&temp) == HA_PROPERTY_TEMPERATURE);

    const ha_zb_state_key_t onoff = key(HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF);
    CHECK(semantics_property_from_key(&onoff) == HA_PROPERTY_POWER);

    const ha_zb_state_key_t unknown = key(0x1234, 0x5678);
    CHECK(semantics_property_from_key(&unknown) == HA_PROPERTY_UNKNOWN);
    CHECK(semantics_property_from_key(NULL) == HA_PROPERTY_UNKNOWN);

    /* нет маппинга → false */
    ha_value_t v = {0};
    const ha_zb_state_record_t s = st(1, HA_ZB_TYPE_UINT8);
    CHECK(!semantics_state_value(&unknown, &s, &v));

    /* тип вне словаря скаляров → false (даже для известной пары) */
    const ha_zb_state_record_t str = st(0, HA_ZB_TYPE_OCTET_STRING);
    CHECK(!semantics_state_value(&temp, &str, &v));

    /* значение несёт kind по дескриптору свойства */
    const ha_zb_state_record_t on = st(1, HA_ZB_TYPE_BOOL);
    CHECK(semantics_state_value(&onoff, &on, &v));
    CHECK(v.kind == HA_VALUE_BOOL);

    const ha_zb_state_record_t t = st(2150, HA_ZB_TYPE_INT16);
    CHECK(semantics_state_value(&temp, &t, &v));
    CHECK(v.kind == HA_VALUE_FLOAT);
}

/* --- Семантический запрос → та же физическая команда, что раньше (фаза 3.5) --- */
static void test_build_command(void)
{
    ha_zb_state_key_t target = {0};
    target.device_uid = 0x00124B000A1B2C3Dull;
    target.endpoint = 2;

    ha_zb_command_t cmd = {0};
    ha_command_value_t cv = {0};

    /* POWER ON/OFF/TOGGLE — без аргументов (args_len 0). */
    CHECK(semantics_build_command(&target, HA_PROPERTY_POWER, HA_ACTION_ON, NULL, &cmd));
    CHECK(cmd.device_uid == target.device_uid && cmd.dst_endpoint == 2);
    CHECK(cmd.cluster_id == HA_ZB_CLUSTER_ON_OFF && cmd.command_id == HA_ZB_CMD_ON_OFF_ON);
    CHECK(cmd.args_len == 0);
    CHECK(semantics_build_command(&target, HA_PROPERTY_POWER, HA_ACTION_OFF, NULL, &cmd));
    CHECK(cmd.command_id == HA_ZB_CMD_ON_OFF_OFF);
    CHECK(semantics_build_command(&target, HA_PROPERTY_POWER, HA_ACTION_TOGGLE, NULL, &cmd));
    CHECK(cmd.command_id == HA_ZB_CMD_ON_OFF_TOGGLE);

    /* BRIGHTNESS SET 70% → 0..254. */
    cv = scalar(70.0f);
    CHECK(semantics_build_command(&target, HA_PROPERTY_BRIGHTNESS, HA_ACTION_SET, &cv, &cmd));
    CHECK(cmd.cluster_id == HA_ZB_CLUSTER_LEVEL_CONTROL);
    CHECK(cmd.command_id == HA_ZB_CMD_LEVEL_MOVE_TO_LEVEL);
    CHECK(cmd.args_len == 3 && cmd.args[0] == 178 && cmd.args[1] == 0 && cmd.args[2] == 0);

    /* COLOR_TEMPERATURE SET 4000 K → mireds 250. */
    cv = scalar(4000.0f);
    CHECK(semantics_build_command(&target, HA_PROPERTY_COLOR_TEMPERATURE, HA_ACTION_SET, &cv, &cmd));
    CHECK(cmd.command_id == HA_ZB_CMD_COLOR_MOVE_TO_COLOR_TEMPERATURE);
    CHECK(cmd.args_len == 4 && cmd.args[0] == 250 && cmd.args[1] == 0);

    /* COLOR_HUE SET 120° → 85 (direction shortest). */
    cv = scalar(120.0f);
    CHECK(semantics_build_command(&target, HA_PROPERTY_COLOR_HUE, HA_ACTION_SET, &cv, &cmd));
    CHECK(cmd.command_id == HA_ZB_CMD_COLOR_MOVE_TO_HUE);
    CHECK(cmd.args_len == 4 && cmd.args[0] == 85 && cmd.args[1] == 0);

    /* COLOR_SATURATION SET 80% → 203. */
    cv = scalar(80.0f);
    CHECK(semantics_build_command(&target, HA_PROPERTY_COLOR_SATURATION, HA_ACTION_SET, &cv, &cmd));
    CHECK(cmd.command_id == HA_ZB_CMD_COLOR_MOVE_TO_SATURATION);
    CHECK(cmd.args_len == 3 && cmd.args[0] == 203);

    /* COLOR SET(x,y) → MoveToColor, xy 0..1 → 0..65535 LE. */
    cv = (ha_command_value_t){.kind = HA_COMMAND_VALUE_XY, .value.xy = {.x = 0.3f, .y = 0.4f}};
    CHECK(semantics_build_command(&target, HA_PROPERTY_COLOR, HA_ACTION_SET, &cv, &cmd));
    CHECK(cmd.command_id == HA_ZB_CMD_COLOR_MOVE_TO_COLOR);
    CHECK(cmd.args_len == 6);
    CHECK(cmd.args[0] == 0xCD && cmd.args[1] == 0x4C); /* 0.3*65535 = 19661 */
    CHECK(cmd.args[2] == 0x66 && cmd.args[3] == 0x66); /* 0.4*65535 = 26214 */

    /* Неверная форма значения / нет маппинга / NULL target. */
    CHECK(!semantics_build_command(&target, HA_PROPERTY_COLOR, HA_ACTION_SET, NULL, &cmd));
    cv = scalar(50.0f);
    CHECK(!semantics_build_command(&target, HA_PROPERTY_COLOR, HA_ACTION_SET, &cv, &cmd));
    CHECK(!semantics_build_command(&target, HA_PROPERTY_TEMPERATURE, HA_ACTION_SET, &cv, &cmd));
    CHECK(!semantics_build_command(NULL, HA_PROPERTY_POWER, HA_ACTION_ON, NULL, &cmd));
}

/* --- События: физический Zigbee event → семантический ha_event_t (4.1) --- */
static void test_decode_event(void)
{
    ha_zb_event_t phys = {0};
    phys.device_uid = 0x00124B000A1B2C3Dull;
    phys.endpoint = 1;
    phys.cluster_id = HA_ZB_CLUSTER_ON_OFF;
    phys.command_id = HA_ZB_CMD_ON_OFF_TOGGLE;

    ha_device_record_t device = {0};
    device.model[0] = 'X';

    ha_event_t ev = {0};
    CHECK(semantics_decode_event(&phys, &device, &ev));
    CHECK(ev.id == HA_EVENT_SINGLE_PRESS);
    CHECK(ev.device_uid == phys.device_uid);
    CHECK(ev.endpoint == 1);
    CHECK(ev.value.kind == HA_VALUE_NONE);

    /* ON/OFF от контроллера — тоже нажатие кнопки. */
    phys.command_id = HA_ZB_CMD_ON_OFF_ON;
    CHECK(semantics_decode_event(&phys, &device, &ev) && ev.id == HA_EVENT_SINGLE_PRESS);
    phys.command_id = HA_ZB_CMD_ON_OFF_OFF;
    CHECK(semantics_decode_event(&phys, &device, &ev) && ev.id == HA_EVENT_SINGLE_PRESS);

    /* неизвестная пара кластер/команда → нет семантики. */
    phys.cluster_id = HA_ZB_CLUSTER_LEVEL_CONTROL;
    phys.command_id = HA_ZB_CMD_ON_OFF_TOGGLE;
    CHECK(!semantics_decode_event(&phys, &device, &ev));

    /* device=NULL — общий профиль. */
    phys.cluster_id = HA_ZB_CLUSTER_ON_OFF;
    CHECK(semantics_decode_event(&phys, NULL, &ev) && ev.id == HA_EVENT_SINGLE_PRESS);

    CHECK(!semantics_decode_event(NULL, &device, &ev));

    /* общий event vocabulary включает системный тик (проверка на этапе компиляции) */
    _Static_assert(HA_EVENT_MINUTE_TICK == 4, "event id ABI");
}

/* --- Capabilities: cluster→properties, property→actions (5.0) --- */
static void test_capabilities(void)
{
    ha_property_id_t props[8];
    ha_action_id_t acts[4];

    size_t n = semantics_cluster_properties(HA_ZB_CLUSTER_ON_OFF, props, 8);
    CHECK(n == 1 && props[0] == HA_PROPERTY_POWER);

    n = semantics_cluster_properties(HA_ZB_CLUSTER_COLOR_CONTROL, props, 8);
    CHECK(n == 5);
    bool has_x = false, has_ct = false;
    for (size_t i = 0; i < n; i++) {
        if (props[i] == HA_PROPERTY_COLOR_X) has_x = true;
        if (props[i] == HA_PROPERTY_COLOR_TEMPERATURE) has_ct = true;
    }
    CHECK(has_x && has_ct);

    CHECK(semantics_cluster_properties(0x1234, props, 8) == 0);
    /* max ограничивает выдачу */
    CHECK(semantics_cluster_properties(HA_ZB_CLUSTER_COLOR_CONTROL, props, 2) == 2);

    n = semantics_property_actions(HA_PROPERTY_POWER, acts, 4);
    CHECK(n == 3 && acts[0] == HA_ACTION_ON && acts[2] == HA_ACTION_TOGGLE);

    n = semantics_property_actions(HA_PROPERTY_BRIGHTNESS, acts, 4);
    CHECK(n == 1 && acts[0] == HA_ACTION_SET);

    n = semantics_property_actions(HA_PROPERTY_COLOR, acts, 4);
    CHECK(n == 1 && acts[0] == HA_ACTION_SET);
}

/* --- Компилятор semantic-правила → physical record (5.3.6) --- */
static void test_compile_automation(void)
{
    const ha_device_uid_t DEV = 0x00124B000A1B2C3Dull;
    ha_automation_record_t rec = {0};

    /* temperature > 25 → POWER ON */
    ha_sem_rule_t rule = {0};
    rule.enabled = 1;
    rule.trigger.kind = HA_TRIGGER_STATE;
    rule.trigger.device_uid = DEV;
    rule.trigger.endpoint = 2;
    rule.trigger.property = HA_PROPERTY_TEMPERATURE;
    rule.trigger.op = HA_CONDITION_OP_GT;
    rule.trigger.edge = HA_TRIGGER_EDGE_ANY;
    rule.trigger.value = 25.0f;
    rule.action.target.device_uid = 0; /* то же устройство */
    rule.action.target.endpoint = 2;
    rule.action.target.property = HA_PROPERTY_POWER;
    rule.action.action = HA_ACTION_ON;
    rule.action.value_kind = HA_COMMAND_VALUE_NONE;
    CHECK(semantics_compile_automation(&rule, NULL, &rec));
    CHECK(rec.enabled == 1 && rec.trigger_kind == HA_TRIGGER_STATE);
    CHECK(rec.trigger_b.state.device_uid == DEV && rec.trigger_b.state.endpoint == 2);
    CHECK(rec.trigger_b.state.cluster_id == HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT);
    CHECK(rec.trigger_b.state.attr_id == HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE);
    CHECK(rec.trigger_a.state_value == 25.0f);
    CHECK(rec.action_device_uid == 0 && rec.action_endpoint == 2);
    CHECK(rec.action_cluster_id == HA_ZB_CLUSTER_ON_OFF && rec.action_command_id == HA_ZB_CMD_ON_OFF_ON);
    CHECK(rec.action_args_len == 0);

    /* POWER == true AND temperature > 25 → BRIGHTNESS 70% */
    rule.trigger.property = HA_PROPERTY_POWER;
    rule.trigger.op = HA_CONDITION_OP_EQ;
    rule.trigger.value = 1.0f;
    rule.conditions_count = 1;
    rule.conditions[0].ref.device_uid = DEV;
    rule.conditions[0].ref.endpoint = 2;
    rule.conditions[0].ref.property = HA_PROPERTY_TEMPERATURE;
    rule.conditions[0].op = HA_CONDITION_OP_GT;
    rule.conditions[0].value = 25.0f;
    rule.action.target.property = HA_PROPERTY_BRIGHTNESS;
    rule.action.action = HA_ACTION_SET;
    rule.action.value_kind = HA_COMMAND_VALUE_SCALAR;
    rule.action.value.kind = HA_VALUE_FLOAT;
    rule.action.value.value.f32 = 70.0f;
    CHECK(semantics_compile_automation(&rule, NULL, &rec));
    CHECK(rec.trigger_b.state.cluster_id == HA_ZB_CLUSTER_ON_OFF);
    CHECK(rec.conditions_count == 1);
    CHECK(rec.conditions[0].cluster_id == HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT);
    CHECK(rec.conditions[0].op == HA_CONDITION_OP_GT && rec.conditions[0].value == 25.0f);
    CHECK(rec.action_cluster_id == HA_ZB_CLUSTER_LEVEL_CONTROL &&
          rec.action_command_id == HA_ZB_CMD_LEVEL_MOVE_TO_LEVEL);
    CHECK(rec.action_args_len == 3 && rec.action_args[0] == 178);

    /* TIME 07:30 → POWER ON */
    ha_sem_rule_t t = {0};
    t.enabled = 1;
    t.trigger.kind = HA_TRIGGER_TIME;
    t.trigger.minutes_of_day = 7 * 60 + 30;
    t.trigger.weekday_mask = 0x7f;
    t.action.target.device_uid = DEV;
    t.action.target.endpoint = 2;
    t.action.target.property = HA_PROPERTY_POWER;
    t.action.action = HA_ACTION_ON;
    CHECK(semantics_compile_automation(&t, NULL, &rec));
    CHECK(rec.trigger_kind == HA_TRIGGER_TIME);
    CHECK(rec.trigger_a.time.minutes_of_day == 450 && rec.trigger_a.time.weekday_mask == 0x7f);

    /* system MINUTE_TICK → POWER ON (event id адресуется напрямую) */
    ha_sem_rule_t sysr = {0};
    sysr.enabled = 1;
    sysr.trigger.kind = HA_TRIGGER_DEVICE_EVENT;
    sysr.trigger.device_uid = HA_SYSTEM_DEVICE_UID;
    sysr.trigger.event_id = HA_EVENT_MINUTE_TICK;
    sysr.action.target.device_uid = DEV;
    sysr.action.target.endpoint = 2;
    sysr.action.target.property = HA_PROPERTY_POWER;
    sysr.action.action = HA_ACTION_ON;
    CHECK(semantics_compile_automation(&sysr, NULL, &rec));
    CHECK(rec.trigger_b.event.device_uid == HA_SYSTEM_DEVICE_UID);
    CHECK(rec.trigger_b.event.command_id == HA_EVENT_MINUTE_TICK);

    ha_device_record_t dev = {0};

    /* кнопка SINGLE_PRESS → TOGGLE: общий профиль неоднозначен → отказ */
    ha_sem_rule_t btn = {0};
    btn.enabled = 1;
    btn.trigger.kind = HA_TRIGGER_DEVICE_EVENT;
    btn.trigger.device_uid = DEV;
    btn.trigger.event_id = HA_EVENT_SINGLE_PRESS;
    btn.action.target.device_uid = DEV;
    btn.action.target.endpoint = 2;
    btn.action.target.property = HA_PROPERTY_POWER;
    btn.action.action = HA_ACTION_TOGGLE;
    CHECK(!semantics_compile_automation(&btn, &dev, &rec));

    /* BETWEEN на STATE-триггере → отказ (нет второго порога в legacy) */
    ha_sem_rule_t btw = {0};
    btw.enabled = 1;
    btw.trigger.kind = HA_TRIGGER_STATE;
    btw.trigger.device_uid = DEV;
    btw.trigger.endpoint = 2;
    btw.trigger.property = HA_PROPERTY_TEMPERATURE;
    btw.trigger.op = HA_CONDITION_OP_BETWEEN;
    btw.trigger.value = 10.0f;
    btw.trigger.value2 = 15.0f;
    btw.action.target.property = HA_PROPERTY_POWER;
    btw.action.action = HA_ACTION_ON;
    CHECK(!semantics_compile_automation(&btw, NULL, &rec));

    uint16_t cmd = 0;
    CHECK(!semantics_event_to_physical(&dev, HA_EVENT_SINGLE_PRESS, &cmd));
}

/* --- Обратная проекция physical → semantic (5.3.4a) --- */
static void test_decompile_automation(void)
{
    const ha_device_uid_t DEV = 0x00124B000A1B2C3Dull;
    ha_sem_rule_t rule = {0};
    rule.enabled = 1;
    rule.trigger.kind = HA_TRIGGER_STATE;
    rule.trigger.device_uid = DEV;
    rule.trigger.endpoint = 2;
    rule.trigger.property = HA_PROPERTY_TEMPERATURE;
    rule.trigger.op = HA_CONDITION_OP_GT;
    rule.trigger.edge = HA_TRIGGER_EDGE_RISING;
    rule.trigger.value = 25.0f;
    rule.conditions_count = 1;
    rule.conditions[0].ref.device_uid = 0;
    rule.conditions[0].ref.endpoint = 2;
    rule.conditions[0].ref.property = HA_PROPERTY_TEMPERATURE;
    rule.conditions[0].op = HA_CONDITION_OP_LT;
    rule.conditions[0].value = 30.0f;
    rule.action.target.device_uid = DEV;
    rule.action.target.endpoint = 2;
    rule.action.target.property = HA_PROPERTY_BRIGHTNESS;
    rule.action.action = HA_ACTION_SET;
    rule.action.value_kind = HA_COMMAND_VALUE_SCALAR;
    rule.action.value.kind = HA_VALUE_FLOAT;
    rule.action.value.value.f32 = 70.0f;

    ha_automation_record_t rec = {0};
    ha_sem_rule_t back = {0};
    CHECK(semantics_compile_automation(&rule, NULL, &rec));
    CHECK(semantics_decompile_automation(&rec, NULL, &back));
    CHECK(back.enabled == 1 && back.trigger.kind == HA_TRIGGER_STATE);
    CHECK(back.trigger.device_uid == DEV && back.trigger.endpoint == 2);
    CHECK(back.trigger.property == HA_PROPERTY_TEMPERATURE);
    CHECK(back.trigger.op == HA_CONDITION_OP_GT && back.trigger.edge == HA_TRIGGER_EDGE_RISING);
    CHECK(back.trigger.value == 25.0f);
    CHECK(back.conditions_count == 1 && back.conditions[0].ref.property == HA_PROPERTY_TEMPERATURE);
    CHECK(back.conditions[0].op == HA_CONDITION_OP_LT && back.conditions[0].value == 30.0f);
    CHECK(back.action.target.property == HA_PROPERTY_BRIGHTNESS &&
          back.action.action == HA_ACTION_SET);
    CHECK(back.action.value_kind == HA_COMMAND_VALUE_SCALAR);
    CHECK_NEAR(back.action.value.value.f32, 70.0f, 0.2f);

    /* TIME round-trip */
    ha_sem_rule_t t = {0};
    t.enabled = 1;
    t.trigger.kind = HA_TRIGGER_TIME;
    t.trigger.minutes_of_day = 450;
    t.trigger.weekday_mask = 0x1f;
    t.action.target.property = HA_PROPERTY_POWER;
    t.action.action = HA_ACTION_ON;
    CHECK(semantics_compile_automation(&t, NULL, &rec));
    CHECK(semantics_decompile_automation(&rec, NULL, &back));
    CHECK(back.trigger.kind == HA_TRIGGER_TIME && back.trigger.minutes_of_day == 450 &&
          back.trigger.weekday_mask == 0x1f);

    /* system MINUTE_TICK round-trip */
    ha_sem_rule_t sysr = {0};
    sysr.enabled = 1;
    sysr.trigger.kind = HA_TRIGGER_DEVICE_EVENT;
    sysr.trigger.device_uid = HA_SYSTEM_DEVICE_UID;
    sysr.trigger.event_id = HA_EVENT_MINUTE_TICK;
    sysr.action.target.property = HA_PROPERTY_POWER;
    sysr.action.action = HA_ACTION_ON;
    CHECK(semantics_compile_automation(&sysr, NULL, &rec));
    CHECK(semantics_decompile_automation(&rec, NULL, &back));
    CHECK(back.trigger.kind == HA_TRIGGER_DEVICE_EVENT &&
          back.trigger.event_id == HA_EVENT_MINUTE_TICK);

    /* zigbee event по command_id однозначен: Toggle(0x02) → SINGLE_PRESS */
    ha_automation_record_t evrec = {0};
    evrec.enabled = 1;
    evrec.trigger_kind = HA_TRIGGER_DEVICE_EVENT;
    evrec.trigger_b.event.device_uid = DEV;
    evrec.trigger_b.event.command_id = 0x02;
    evrec.action_cluster_id = HA_ZB_CLUSTER_ON_OFF;
    evrec.action_command_id = HA_ZB_CMD_ON_OFF_TOGGLE;
    ha_device_record_t dev = {0};
    CHECK(semantics_decompile_automation(&evrec, &dev, &back));
    CHECK(back.trigger.kind == HA_TRIGGER_DEVICE_EVENT &&
          back.trigger.event_id == HA_EVENT_SINGLE_PRESS);

    /* неизвестная команда события → невыразимо */
    ha_automation_record_t unkev = evrec;
    unkev.trigger_b.event.command_id = 0x42;
    CHECK(!semantics_decompile_automation(&unkev, &dev, &back));

    /* неизвестный action → невыразимо */
    ha_automation_record_t unk = evrec;
    unk.trigger_kind = HA_TRIGGER_TIME;
    unk.action_cluster_id = 0x1234;
    CHECK(!semantics_decompile_automation(&unk, NULL, &back));
}

int main(void)
{
    test_parity();
    test_normalization();
    test_identity_and_failures();
    test_property_key();
    test_build_command();
    test_decode_event();
    test_capabilities();
    test_compile_automation();
    test_decompile_automation();

    if (g_failures == 0) {
        printf("all semantics tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
