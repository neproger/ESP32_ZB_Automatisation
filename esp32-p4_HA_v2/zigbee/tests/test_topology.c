#include "zigbee/zigbee_device_store.h"

#include <stdio.h>
#include <string.h>

/*
 * Топология: endpoint и состав кластеров (docs/services/ZIGBEE.md §3). Проверяется, что
 * состав доезжает до RAM-модели адаптера целиком, повтор не множит записи, а устройство
 * появляется вместе с первым endpoint'ом.
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
    const zigbee_endpoint_desc_t desc = endpoint_desc(1);
    bool changed = false;
    CHECK(sys_ok(zigbee_topology_apply(&desc, &changed)));
    CHECK(changed);

    /* Устройство появилось вместе с первым endpoint'ом. */
    CHECK(zigbee_device_has(UID));

    ha_endpoint_record_t record = {0};
    CHECK(zigbee_topology_get(UID, 1, &record));
    CHECK(record.profile_id == HA_ZB_PROFILE_HA);
    CHECK(record.device_id == 0x0101u);
    CHECK(record.cluster_count == 2);
    CHECK(record.clusters[0].cluster_id == HA_ZB_CLUSTER_ON_OFF);
    CHECK(record.clusters[0].role == HA_ZB_ROLE_SERVER);
    CHECK(record.clusters[1].cluster_id == HA_ZB_CLUSTER_LEVEL_CONTROL);

    /* Повтор того же состава — не изменение. */
    CHECK(sys_ok(zigbee_topology_apply(&desc, &changed)));
    CHECK(!changed);

    zigbee_device_remove(UID);
}

static void test_endpoints_are_independent(void)
{
    const zigbee_endpoint_desc_t first = endpoint_desc(1);
    const zigbee_endpoint_desc_t second = endpoint_desc(2);
    bool changed = false;
    CHECK(sys_ok(zigbee_topology_apply(&first, &changed)));
    CHECK(sys_ok(zigbee_topology_apply(&second, &changed)));

    ha_endpoint_record_t record = {0};
    CHECK(zigbee_topology_get(UID, 1, &record));
    CHECK(zigbee_topology_get(UID, 2, &record));
    CHECK(!zigbee_topology_get(UID, 3, &record));

    zigbee_device_remove(UID);
    CHECK(!zigbee_topology_get(UID, 1, &record)); /* устройство ушло вместе с endpoint'ами */
}

static void test_malformed_topology_is_rejected(void)
{
    bool changed = false;
    CHECK(sys_is(zigbee_topology_apply(NULL, &changed), SYS_CODE_INVALID_ARG));

    const zigbee_endpoint_desc_t no_clusters = endpoint_desc(1);
    CHECK(sys_is(zigbee_topology_apply(
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
    CHECK(sys_is(zigbee_topology_apply(&oversized, &changed), SYS_CODE_INVALID_ARG));

    const zigbee_endpoint_desc_t ok = no_clusters;
    CHECK(sys_ok(zigbee_topology_apply(&ok, &changed)));

    zigbee_device_remove(UID);
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
