#include "zigbee/zigbee_topology.h"

#include <stdio.h>
#include <string.h>

#include "domain/domain.h"
#include "ha_model/ha_entities.h"
#include "mstore_platform.h"
#include "nor_sim.h"
#include "nor_sim_device.h"
#include "zigbee/zigbee_state.h"

/*
 * Топология: endpoint и состав кластеров (docs/services/ZIGBEE.md §3). Проверяется, что
 * состав доезжает до Domain целиком, повтор не множит записи, а устройство появляется
 * вместе с первым endpoint'ом.
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

static const ha_cluster_entry_t CLUSTERS[] = {
    {.cluster_id = HA_ZB_CLUSTER_ON_OFF, .role = HA_ZB_ROLE_SERVER},
    {.cluster_id = HA_ZB_CLUSTER_LEVEL_CONTROL, .role = HA_ZB_ROLE_SERVER},
};

static zigbee_endpoint_desc_t endpoint_desc(uint8_t endpoint)
{
    const zigbee_endpoint_desc_t desc = {
        .device_uid = UID,
        .endpoint = endpoint,
        .profile_id = HA_ZB_PROFILE_HA,
        .device_id = 0x0101u,
        .cluster_count = (uint8_t)(sizeof(CLUSTERS) / sizeof(CLUSTERS[0])),
        .clusters = CLUSTERS,
    };
    return desc;
}

static void test_topology_is_written_once(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 3, 16, 8, 64)));
    register_types(&domain);

    const zigbee_endpoint_desc_t desc = endpoint_desc(1);
    bool changed = false;
    CHECK(sys_ok(zigbee_topology_apply(&domain, &desc, &changed)));
    CHECK(changed);

    /* Устройство появилось вместе с первым endpoint'ом. */
    ha_device_record_t device_record = {0};
    CHECK(sys_ok(domain_entity_get(&domain, (domain_entity_t)HA_ENTITY_DEVICE, &UID,
                                   &device_record)));

    ha_endpoint_key_t key = {0};
    key.device_uid = UID;
    key.endpoint = 1;
    ha_endpoint_record_t record = {0};
    CHECK(sys_ok(domain_entity_get(&domain, (domain_entity_t)HA_ENTITY_ENDPOINT, &key, &record)));
    CHECK(record.profile_id == HA_ZB_PROFILE_HA);
    CHECK(record.device_id == 0x0101u);
    CHECK(record.cluster_count == 2);
    CHECK(record.clusters[0].cluster_id == HA_ZB_CLUSTER_ON_OFF);
    CHECK(record.clusters[0].role == HA_ZB_ROLE_SERVER);
    CHECK(record.clusters[1].cluster_id == HA_ZB_CLUSTER_LEVEL_CONTROL);

    /* Повтор того же состава — не изменение. */
    CHECK(sys_ok(zigbee_topology_apply(&domain, &desc, &changed)));
    CHECK(!changed);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_ENDPOINT) == 1);
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_DEVICE) == 1);

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

static void test_endpoints_are_independent(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 3, 16, 8, 64)));
    register_types(&domain);

    const zigbee_endpoint_desc_t first = endpoint_desc(1);
    const zigbee_endpoint_desc_t second = endpoint_desc(2);
    bool changed = false;
    CHECK(sys_ok(zigbee_topology_apply(&domain, &first, &changed)));
    CHECK(sys_ok(zigbee_topology_apply(&domain, &second, &changed)));
    CHECK(record_count(&domain, (domain_entity_t)HA_ENTITY_ENDPOINT) == 2);

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

static void test_malformed_topology_is_rejected(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 3, 16, 8, 64)));
    register_types(&domain);

    bool changed = false;
    CHECK(sys_is(zigbee_topology_apply(&domain, NULL, &changed), SYS_CODE_INVALID_ARG));

    const zigbee_endpoint_desc_t no_clusters = endpoint_desc(1);
    CHECK(sys_is(zigbee_topology_apply(
                     &domain,
                     &(zigbee_endpoint_desc_t){.device_uid = UID, .endpoint = 1,
                                               .cluster_count = 0, .clusters = CLUSTERS},
                     &changed),
                 SYS_CODE_INVALID_ARG));
    CHECK(!changed);

    const zigbee_endpoint_desc_t oversized = {
        .device_uid = UID,
        .endpoint = 1,
        .cluster_count = HA_ENDPOINT_CLUSTERS_MAX + 1,
        .clusters = CLUSTERS,
    };
    CHECK(sys_is(zigbee_topology_apply(&domain, &oversized, &changed), SYS_CODE_INVALID_ARG));

    const zigbee_endpoint_desc_t ok = no_clusters;
    CHECK(sys_ok(zigbee_topology_apply(&domain, &ok, &changed)));

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

int main(void)
{
    test_topology_is_written_once();
    test_endpoints_are_independent();
    test_malformed_topology_is_rejected();

    if (g_failures != 0) {
        printf("test_topology: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_topology: OK\n");
    return 0;
}
