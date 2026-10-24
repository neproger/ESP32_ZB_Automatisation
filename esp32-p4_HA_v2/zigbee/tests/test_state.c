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
    CHECK(sys_ok(zigbee_device_set_identity(UID, "_TZ3000_test", "TS0001", &changed)));
    CHECK(changed);
    CHECK(zigbee_device_has(UID));
    CHECK(zigbee_device_get(UID, &record));
    CHECK(strcmp(record.model, "TS0001") == 0);

    /* Повтор той же модели — не изменение. */
    CHECK(sys_ok(zigbee_device_set_identity(UID, "_TZ3000_test", "TS0001", &changed)));
    CHECK(!changed);

    /* Повторное интервью обновляет модель. */
    CHECK(sys_ok(zigbee_device_set_identity(UID, "_TZ3000_test", "TS0002", &changed)));
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
    CHECK(sys_is(zigbee_device_set_identity(UID, NULL, "X", &changed), SYS_CODE_INVALID_ARG));
    CHECK(sys_is(zigbee_device_set_identity(UID, "X", NULL), SYS_CODE_INVALID_ARG));
    CHECK(!zigbee_device_get(UID, NULL));
}

int main(void)
{
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
