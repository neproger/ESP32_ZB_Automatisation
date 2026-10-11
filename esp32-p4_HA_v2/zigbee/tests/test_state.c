#include "zigbee/zigbee_state.h"

#include <stdio.h>
#include <string.h>

#include "zigbee/zigbee_device_store.h"

/*
 * Чистая часть сервиса (docs/services/ZIGBEE.md §3.1, §4): проекция репорта в координату
 * и компактное значение, плюс Zigbee-private RAM-модель устройства (имя/модель).
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static const ha_device_uid_t UID = 0x00124B000A1B2C3Dull;

static zigbee_report_t report_of(uint16_t cluster_id, uint16_t attr_id, uint8_t zcl_type,
                                 uint32_t raw)
{
    zigbee_report_t report = {0};
    report.device_uid = UID;
    report.cluster_id = cluster_id;
    report.attr_id = attr_id;
    report.endpoint = 1;
    report.zcl_type = zcl_type;
    report.raw = raw;
    return report;
}

static void test_value_mapping(void)
{
    const zigbee_report_t boolean = report_of(HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF,
                                              HA_ZB_TYPE_BOOL, 1);
    const domain_value_t boolean_value = zigbee_value_compact(&boolean);
    CHECK(boolean_value.type == (uint8_t)DOMAIN_VALUE_U32);
    CHECK(boolean_value.v.u32 == 1u);

    const zigbee_report_t level = report_of(HA_ZB_CLUSTER_LEVEL_CONTROL,
                                            HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, HA_ZB_TYPE_UINT8, 254);
    const domain_value_t level_value = zigbee_value_compact(&level);
    CHECK(level_value.type == (uint8_t)DOMAIN_VALUE_U32);
    CHECK(level_value.v.u32 == 254u);

    /* Отрицательное значение приходит расширенным по знаку: int16 -25. */
    const zigbee_report_t temperature = report_of(HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
                                                  HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE,
                                                  HA_ZB_TYPE_INT16, (uint32_t)-25);
    const domain_value_t temperature_value = zigbee_value_compact(&temperature);
    CHECK(temperature_value.type == (uint8_t)DOMAIN_VALUE_I32);
    CHECK(temperature_value.v.i32 == -25);

    float as_float = 21.5f;
    uint32_t float_bits = 0;
    memcpy(&float_bits, &as_float, sizeof(float_bits));
    const zigbee_report_t measured = report_of(HA_ZB_CLUSTER_RELATIVE_HUMIDITY,
                                               HA_ZB_ATTR_HUMIDITY_MEASURED_VALUE,
                                               HA_ZB_TYPE_SINGLE_FLOAT, float_bits);
    const domain_value_t measured_value = zigbee_value_compact(&measured);
    CHECK(measured_value.type == (uint8_t)DOMAIN_VALUE_F32);
    CHECK(measured_value.v.f32 == as_float);

    const zigbee_report_t enum_value = report_of(HA_ZB_CLUSTER_COLOR_CONTROL,
                                                 HA_ZB_ATTR_COLOR_COLOR_MODE, HA_ZB_TYPE_ENUM8, 3);
    const domain_value_t mode = zigbee_value_compact(&enum_value);
    CHECK(mode.type == (uint8_t)DOMAIN_VALUE_ENUM);
    CHECK(mode.v.u32 == 3u);

    /* Тип вне словаря скаляров не изобретает значение: факт остаётся без value. */
    const zigbee_report_t text = report_of(HA_ZB_CLUSTER_BASIC, 0x0004u, HA_ZB_TYPE_CHAR_STRING, 0);
    CHECK(zigbee_value_compact(&text).type == (uint8_t)DOMAIN_VALUE_NONE);
}

static void test_key_matches_report(void)
{
    const zigbee_report_t report = report_of(HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF,
                                             HA_ZB_TYPE_BOOL, 1);
    ha_zb_state_key_t key = {0};
    zigbee_state_key_build(&report, &key);

    CHECK(key.device_uid == UID);
    CHECK(key.cluster_id == HA_ZB_CLUSTER_ON_OFF);
    CHECK(key.attr_id == HA_ZB_ATTR_ON_OFF_ON_OFF);
    CHECK(key.endpoint == 1);
}

/* Zigbee-private RAM-модель: устройство появляется по интервью, модель обновляется. */
static void test_device_model_store(void)
{
    bool changed = false;
    ha_device_record_t record = {0};

    CHECK(!zigbee_device_has(UID));
    CHECK(sys_ok(zigbee_device_set_model(UID, "TS0001", &changed)));
    CHECK(changed);
    CHECK(zigbee_device_has(UID));
    CHECK(zigbee_device_get(UID, &record));
    CHECK(strcmp(record.model, "TS0001") == 0);

    /* Повтор той же модели — не изменение. */
    CHECK(sys_ok(zigbee_device_set_model(UID, "TS0001", &changed)));
    CHECK(!changed);

    /* Повторное интервью обновляет модель. */
    CHECK(sys_ok(zigbee_device_set_model(UID, "TS0002", &changed)));
    CHECK(changed);
    CHECK(zigbee_device_get(UID, &record));
    CHECK(strcmp(record.model, "TS0002") == 0);

    CHECK(sys_ok(zigbee_device_remove(UID)));
    CHECK(!zigbee_device_has(UID));
    CHECK(sys_ok(zigbee_device_remove(UID))); /* повтор — не ошибка */
}

static void test_store_rejects_bad_args(void)
{
    bool changed = false;
    CHECK(sys_is(zigbee_device_set_model(UID, NULL, &changed), SYS_CODE_INVALID_ARG));
    CHECK(sys_is(zigbee_device_set_model(UID, "X", NULL), SYS_CODE_INVALID_ARG));
    CHECK(!zigbee_device_get(UID, NULL));
}

int main(void)
{
    test_value_mapping();
    test_key_matches_report();
    test_device_model_store();
    test_store_rejects_bad_args();

    if (g_failures != 0) {
        printf("test_state: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_state: OK\n");
    return 0;
}
