#include "zigbee/zigbee_interview.h"

#include <stdio.h>
#include <string.h>

#include "domain/domain.h"
#include "ha_model/ha_entities.h"
#include "mstore_platform.h"
#include "nor_sim.h"
#include "nor_sim_device.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_state.h"

/*
 * Интервью: роли кластеров из Simple Descriptor и запись устройства с топологией
 * (docs/services/ZIGBEE.md §3.1, §9.4).
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

/* Ёмкости и backing повторяют bootstrap приложения (main/app_main.c). */
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
    CHECK(sys_ok(domain_register_entity(domain, &state)));

    domain_entity_desc_t endpoint = {0};
    endpoint.type = (domain_entity_t)HA_ENTITY_ENDPOINT;
    endpoint.key_size = sizeof(ha_endpoint_key_t);
    endpoint.payload_size = sizeof(ha_endpoint_record_t);
    endpoint.capacity = 128;
    endpoint.backing = DOMAIN_BACKING_RAM;
    CHECK(sys_ok(domain_register_entity(domain, &endpoint)));
}

static void test_cluster_roles(void)
{
    const uint16_t input[] = {HA_ZB_CLUSTER_BASIC, HA_ZB_CLUSTER_ON_OFF};
    const uint16_t output[] = {HA_ZB_CLUSTER_IDENTIFY};
    ha_cluster_entry_t entries[HA_ENDPOINT_CLUSTERS_MAX] = {0};
    uint8_t count = 0;
    CHECK(sys_ok(zigbee_clusters_from_lists(input, 2, output, 1, entries,
                                            HA_ENDPOINT_CLUSTERS_MAX, &count)));
    CHECK(count == 3);
    CHECK(entries[0].cluster_id == HA_ZB_CLUSTER_BASIC);
    CHECK(entries[0].role == HA_ZB_ROLE_SERVER);
    CHECK(entries[1].cluster_id == HA_ZB_CLUSTER_ON_OFF);
    CHECK(entries[1].role == HA_ZB_ROLE_SERVER);
    CHECK(entries[2].cluster_id == HA_ZB_CLUSTER_IDENTIFY);
    CHECK(entries[2].role == HA_ZB_ROLE_CLIENT);

    /* Больше лимита: состав не обрезается молча — отказ. */
    const uint16_t too_many[HA_ENDPOINT_CLUSTERS_MAX + 1] = {0};
    CHECK(sys_is(zigbee_clusters_from_lists(too_many, HA_ENDPOINT_CLUSTERS_MAX + 1, NULL, 0,
                                            entries, HA_ENDPOINT_CLUSTERS_MAX, &count),
                 SYS_CODE_INVALID_ARG));
    /* Пустой Simple Descriptor — тоже отказ, а не пустая топология. */
    CHECK(sys_is(zigbee_clusters_from_lists(NULL, 0, NULL, 0, entries,
                                            HA_ENDPOINT_CLUSTERS_MAX, &count),
                 SYS_CODE_INVALID_ARG));
}

static void test_interview_writes_device_and_topology(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 3, 16, 8, 64)));
    register_types(&domain);

    zigbee_interview_result_t result = {0};
    result.uid = UID;
    snprintf(result.model, sizeof(result.model), "%s", "TS0201");
    result.endpoint_count = 1;
    result.endpoints[0].endpoint = 1;
    result.endpoints[0].profile_id = HA_ZB_PROFILE_HA;
    result.endpoints[0].device_id = 0x0302u;
    result.endpoints[0].cluster_count = 1;
    result.endpoints[0].clusters[0].cluster_id = HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT;
    result.endpoints[0].clusters[0].role = HA_ZB_ROLE_SERVER;

    CHECK(sys_ok(zigbee_interview_apply(&domain, &result)));

    ha_device_record_t device_record = {0};
    CHECK(sys_ok(domain_entity_get(&domain, (domain_entity_t)HA_ENTITY_DEVICE, &UID,
                                   &device_record)));
    CHECK(strcmp(device_record.model, "TS0201") == 0);

    ha_endpoint_key_t key = {0};
    key.device_uid = UID;
    key.endpoint = 1;
    ha_endpoint_record_t endpoint = {0};
    CHECK(sys_ok(domain_entity_get(&domain, (domain_entity_t)HA_ENTITY_ENDPOINT, &key,
                                   &endpoint)));
    CHECK(endpoint.profile_id == HA_ZB_PROFILE_HA);
    CHECK(endpoint.device_id == 0x0302u);
    CHECK(endpoint.cluster_count == 1);
    CHECK(endpoint.clusters[0].cluster_id == HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT);
    CHECK(endpoint.clusters[0].role == HA_ZB_ROLE_SERVER);

    /* Повтор интервью не множит записи. */
    CHECK(sys_ok(zigbee_interview_apply(&domain, &result)));

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

/* Устройство ушло: снимаются device, topology и state; повтор не ошибка. */
static void test_leave_removes_entities(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 3, 16, 8, 64)));
    register_types(&domain);

    zigbee_interview_result_t result = {0};
    result.uid = UID;
    result.endpoint_count = 1;
    result.endpoints[0].endpoint = 1;
    result.endpoints[0].cluster_count = 1;
    result.endpoints[0].clusters[0].cluster_id = HA_ZB_CLUSTER_ON_OFF;
    result.endpoints[0].clusters[0].role = HA_ZB_ROLE_SERVER;
    CHECK(sys_ok(zigbee_interview_apply(&domain, &result)));

    const zigbee_report_t report = {
        .device_uid = UID,
        .cluster_id = HA_ZB_CLUSTER_ON_OFF,
        .attr_id = HA_ZB_ATTR_ON_OFF_ON_OFF,
        .endpoint = 1,
        .zcl_type = HA_ZB_TYPE_BOOL,
        .raw = 1,
    };
    bool changed = false;
    CHECK(sys_ok(zigbee_state_apply(&domain, &report, &changed)));

    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_DEVICE) == 1);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_ENDPOINT) == 1);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_STATE) == 1);

    CHECK(sys_ok(zigbee_device_remove(&domain, UID)));
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_DEVICE) == 0);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_ENDPOINT) == 0);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_STATE) == 0);

    CHECK(sys_ok(zigbee_device_remove(&domain, UID))); /* повтор — не ошибка */

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

int main(void)
{
    test_cluster_roles();
    test_interview_writes_device_and_topology();
    test_leave_removes_entities();

    if (g_failures != 0) {
        printf("test_interview: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_interview: OK\n");
    return 0;
}
