#include "zigbee/zigbee_topology.h"

#include <string.h>

#include "zigbee/zigbee_state.h"

/*
 * Топология приходит из Simple Descriptor во время интервью и переоткрывается на
 * каждом старте, поэтому сущность живёт в RAM (политика задаётся в bootstrap).
 *
 * Номер endpoint'а в записи не дублируется: он часть ключа.
 */

static sys_error_t zigbee_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_ZIGBEE, code);
}

sys_error_t zigbee_topology_apply(domain_t *domain, const zigbee_endpoint_desc_t *desc,
                                  bool *out_changed)
{
    if (domain == NULL || desc == NULL || out_changed == NULL || desc->clusters == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (desc->cluster_count == 0 || desc->cluster_count > HA_ENDPOINT_CLUSTERS_MAX) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    *out_changed = false;

    const sys_error_t device = zigbee_device_ensure(domain, desc->device_uid);
    if (sys_failed(device)) {
        return device;
    }

    ha_endpoint_key_t key = {0};
    key.device_uid = desc->device_uid;
    key.endpoint = desc->endpoint;

    ha_endpoint_record_t record = {0};
    record.profile_id = desc->profile_id;
    record.device_id = desc->device_id;
    record.cluster_count = desc->cluster_count;
    memcpy(record.clusters, desc->clusters, desc->cluster_count * sizeof(ha_cluster_entry_t));

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;

    return domain_entity_put(domain, (domain_entity_t)HA_ENTITY_ENDPOINT, &key, &record, &meta,
                             out_changed);
}
