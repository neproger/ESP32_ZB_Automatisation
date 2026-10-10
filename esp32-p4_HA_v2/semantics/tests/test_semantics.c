#include "semantics/semantics.h"

#include <math.h>
#include <stdio.h>

#include "ha_model/ha_properties.h"
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

int main(void)
{
    test_parity();
    test_normalization();
    test_identity_and_failures();

    if (g_failures == 0) {
        printf("all semantics tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
