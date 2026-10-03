#include "zigbee/zigbee_state.h"

#include <stdio.h>
#include <string.h>

#include "domain/domain.h"
#include "ha_model/ha_entities.h"
#include "mstore_platform.h"
#include "nor_sim.h"
#include "nor_sim_device.h"
#include "storage/mstore_storage.h"

/*
 * Проверяется чистая часть сервиса: репорт → ключ, запись, компактное значение и
 * правило появления устройства (docs/services/ZIGBEE.md §3.1, §4).
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define SIM_SIZE (64u * 1024u)
#define SIM_ERASE_SIZE 4096u

static const ha_device_uid_t UID = 0x00124B000A1B2C3Dull;

typedef struct {
    size_t seen;
} count_ctx_t;

static bool count_cb(const void *key, const void *record, void *ctx)
{
    (void)key;
    (void)record;
    ((count_ctx_t *)ctx)->seen++;
    return true;
}

static size_t record_count(domain_t *domain, domain_entity_t type)
{
    count_ctx_t ctx = {0};
    CHECK(sys_ok(domain_entity_iter(domain, type, count_cb, &ctx)));
    return ctx.seen;
}

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

static void flash_on(mstore_nor_sim_t **out_sim, nor_sim_device_t *device)
{
    *out_sim = mstore_nor_sim_create(SIM_SIZE, SIM_ERASE_SIZE);
    CHECK(*out_sim != NULL);
    nor_sim_device_init(device, *out_sim);
    mstore_platform_flash_set_device(&device->base);
}

static void flash_off(mstore_nor_sim_t *sim)
{
    mstore_platform_flash_set_device(NULL);
    mstore_nor_sim_destroy(sim);
}

/*
 * Ёмкости и backing — политика bootstrap приложения (main/app_main.c); тест
 * повторяет её, иначе проверялась бы не та геометрия.
 */
static void register_types(domain_t *domain)
{
    domain_entity_desc_t device = {0};
    device.type = (domain_entity_t)HA_ENTITY_DEVICE;
    device.key_size = sizeof(ha_device_uid_t);
    device.payload_size = sizeof(ha_device_record_t);
    device.capacity = 32;
    device.backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH;
    device.persist_key = "device";
    CHECK(sys_ok(domain_register_entity(domain, &device)));

    domain_entity_desc_t state = {0};
    state.type = (domain_entity_t)HA_ENTITY_STATE;
    state.key_size = sizeof(ha_zb_state_key_t);
    state.payload_size = sizeof(ha_zb_state_record_t);
    state.capacity = 256;
    state.backing = DOMAIN_BACKING_RAM;
    state.persist_key = NULL;
    CHECK(sys_ok(domain_register_entity(domain, &state)));
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

/* Репорт от устройства без интервью — не факт: устройства в Domain ещё нет (§9.4). */
static void test_report_without_interview_is_dropped(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 16, 8, 64)));
    register_types(&domain);

    const zigbee_report_t report = report_of(HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF,
                                             HA_ZB_TYPE_BOOL, 1);
    bool changed = false;
    CHECK(sys_is(zigbee_state_apply(&domain, &report, &changed), SYS_CODE_NOT_FOUND));
    CHECK(!changed);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_DEVICE) == 0);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_STATE) == 0);

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

static void test_interviewed_device_gets_state(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 16, 8, 64)));
    register_types(&domain);

    /* Интервью завершилось: устройство с моделью попало в Domain. */
    bool changed = false;
    CHECK(sys_ok(zigbee_device_set_model(&domain, UID, "TS0001", &changed)));
    CHECK(changed);

    const zigbee_report_t report = report_of(HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF,
                                             HA_ZB_TYPE_BOOL, 1);
    CHECK(sys_ok(zigbee_state_apply(&domain, &report, &changed)));
    CHECK(changed);

    ha_device_record_t device_record = {0};
    CHECK(sys_ok(domain_entity_get(&domain, (domain_entity_t)HA_ENTITY_DEVICE, &UID,
                                   &device_record)));
    CHECK(strcmp(device_record.model, "TS0001") == 0);

    ha_zb_state_key_t key = {0};
    zigbee_state_key_build(&report, &key);
    ha_zb_state_record_t state_record = {0};
    CHECK(sys_ok(domain_entity_get(&domain, (domain_entity_t)HA_ENTITY_STATE, &key,
                                   &state_record)));
    CHECK(state_record.raw == 1u);
    CHECK(state_record.zcl_type == HA_ZB_TYPE_BOOL);

    /* Повтор того же значения — не изменение: запись и устройство не множатся. */
    CHECK(sys_ok(zigbee_state_apply(&domain, &report, &changed)));
    CHECK(!changed);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_DEVICE) == 1);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_STATE) == 1);

    /* Повторное интервью обновляет модель, не множа запись. */
    CHECK(sys_ok(zigbee_device_set_model(&domain, UID, "TS0002", &changed)));
    CHECK(changed);
    CHECK(sys_ok(domain_entity_get(&domain, (domain_entity_t)HA_ENTITY_DEVICE, &UID,
                                   &device_record)));
    CHECK(strcmp(device_record.model, "TS0002") == 0);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_DEVICE) == 1);

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

static void test_attributes_are_independent(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 16, 8, 64)));
    register_types(&domain);

    bool changed = false;
    CHECK(sys_ok(zigbee_device_set_model(&domain, UID, "TS0001", &changed)));

    const zigbee_report_t on = report_of(HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF,
                                         HA_ZB_TYPE_BOOL, 1);
    const zigbee_report_t level = report_of(HA_ZB_CLUSTER_LEVEL_CONTROL,
                                            HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, HA_ZB_TYPE_UINT8, 127);
    const zigbee_report_t temperature = report_of(HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
                                                  HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE,
                                                  HA_ZB_TYPE_INT16, 2153);
    CHECK(sys_ok(zigbee_state_apply(&domain, &on, &changed)));
    CHECK(changed);
    CHECK(sys_ok(zigbee_state_apply(&domain, &level, &changed)));
    CHECK(changed);
    CHECK(sys_ok(zigbee_state_apply(&domain, &temperature, &changed)));
    CHECK(changed);

    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_STATE) == 3);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_DEVICE) == 1);

    /* Другой атрибут меняется независимо: число записей не растёт. */
    const zigbee_report_t level_again = report_of(HA_ZB_CLUSTER_LEVEL_CONTROL,
                                                  HA_ZB_ATTR_LEVEL_CURRENT_LEVEL,
                                                  HA_ZB_TYPE_UINT8, 40);
    CHECK(sys_ok(zigbee_state_apply(&domain, &level_again, &changed)));
    CHECK(changed);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_STATE) == 3);

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

static void test_device_persists_but_state_does_not(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 16, 8, 64)));
    register_types(&domain);

    bool changed = false;
    CHECK(sys_ok(zigbee_device_set_model(&domain, UID, "TS0001", &changed)));

    const zigbee_report_t report = report_of(HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF,
                                             HA_ZB_TYPE_BOOL, 1);
    CHECK(sys_ok(zigbee_state_apply(&domain, &report, &changed)));
    CHECK(sys_ok(domain_deinit(&domain)));

    domain_t reopened = {0};
    CHECK(sys_ok(domain_init(&reopened, 2, 16, 8, 64)));
    register_types(&reopened);

    ha_device_record_t device_record = {0};
    CHECK(sys_ok(domain_entity_get(&reopened, (domain_entity_t)HA_ENTITY_DEVICE, &UID,
                                   &device_record)));

    /* Состояние — RAM: после перезагрузки его приносят репорты, а не хранилище. */
    ha_zb_state_key_t key = {0};
    zigbee_state_key_build(&report, &key);
    ha_zb_state_record_t state_record = {0};
    CHECK(sys_is(domain_entity_get(&reopened, (domain_entity_t)HA_ENTITY_STATE, &key,
                                   &state_record),
                 SYS_CODE_NOT_FOUND));

    CHECK(sys_ok(domain_deinit(&reopened)));
    flash_off(sim);
}

/*
 * Размер раздела — следствие геометрии таблиц (MSTORE_FLASH_REGIONS.md §4). Только
 * устройство живёт во flash, состояние — в RAM, поэтому в расчёте одна таблица.
 */
static void test_geometry_fits_partition(void)
{
    size_t region = 0;
    CHECK(sys_ok(mstore_storage_flash_region_size(32, sizeof(ha_device_uid_t),
                                                  sizeof(ha_device_record_t), SIM_ERASE_SIZE,
                                                  &region)));

    /* Раздел mstore в partitions.csv: 0x40000; directory занимает 2 erase-блока. */
    CHECK(region + 2 * SIM_ERASE_SIZE <= 0x40000u);
}

int main(void)
{
    test_value_mapping();
    test_key_matches_report();
    test_report_without_interview_is_dropped();
    test_interviewed_device_gets_state();
    test_attributes_are_independent();
    test_device_persists_but_state_does_not();
    test_geometry_fits_partition();

    if (g_failures != 0) {
        printf("test_state: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_state: OK\n");
    return 0;
}
