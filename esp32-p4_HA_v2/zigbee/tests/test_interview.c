#include "zigbee/zigbee_interview.h"

#include <stdio.h>
#include <string.h>

#include "zigbee/zigbee_device_store.h"

/*
 * Интервью: роли кластеров из Simple Descriptor и запись устройства с топологией в
 * RAM-модель адаптера (docs/services/ZIGBEE.md §3.1, §9.4).
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

    CHECK(sys_ok(zigbee_interview_apply(&result)));

    ha_device_record_t device_record = {0};
    CHECK(zigbee_device_get(UID, &device_record));
    CHECK(strcmp(device_record.model, "TS0201") == 0);

    ha_endpoint_record_t endpoint = {0};
    CHECK(zigbee_topology_get(UID, 1, &endpoint));
    CHECK(endpoint.profile_id == HA_ZB_PROFILE_HA);
    CHECK(endpoint.device_id == 0x0302u);
    CHECK(endpoint.cluster_count == 1);
    CHECK(endpoint.clusters[0].cluster_id == HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT);
    CHECK(endpoint.clusters[0].role == HA_ZB_ROLE_SERVER);

    /* Повтор интервью не множит записи. */
    CHECK(sys_ok(zigbee_interview_apply(&result)));
    CHECK(zigbee_device_has(UID));

    zigbee_device_remove(UID);
}

/* Устройство ушло: снимаются device и topology; повтор не ошибка. */
static void test_leave_removes_entities(void)
{
    zigbee_interview_result_t result = {0};
    result.uid = UID;
    result.endpoint_count = 1;
    result.endpoints[0].endpoint = 1;
    result.endpoints[0].cluster_count = 1;
    result.endpoints[0].clusters[0].cluster_id = HA_ZB_CLUSTER_ON_OFF;
    result.endpoints[0].clusters[0].role = HA_ZB_ROLE_SERVER;
    CHECK(sys_ok(zigbee_interview_apply(&result)));

    CHECK(zigbee_device_has(UID));
    ha_endpoint_record_t endpoint = {0};
    CHECK(zigbee_topology_get(UID, 1, &endpoint));

    CHECK(sys_ok(zigbee_device_remove(UID)));
    CHECK(!zigbee_device_has(UID));
    CHECK(!zigbee_topology_get(UID, 1, &endpoint));

    CHECK(sys_ok(zigbee_device_remove(UID))); /* повтор — не ошибка */
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
