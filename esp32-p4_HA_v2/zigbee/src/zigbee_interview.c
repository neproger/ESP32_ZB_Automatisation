#include "zigbee/zigbee_interview.h"

#include "zigbee/zigbee_state.h"
#include "zigbee/zigbee_topology.h"

/*
 * Чистая часть интервью: ответы радио → топология Domain. Само интервью (ZDO-запросы
 * и колбэки) живёт в zigbee_radio.c, потому что говорит с радио.
 */

static sys_error_t zigbee_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_ZIGBEE, code);
}

sys_error_t zigbee_clusters_from_lists(const uint16_t *input, uint8_t input_count,
                                       const uint16_t *output, uint8_t output_count,
                                       ha_cluster_entry_t *out, uint8_t out_max,
                                       uint8_t *out_count)
{
    if (out == NULL || out_count == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if ((input_count != 0 && input == NULL) || (output_count != 0 && output == NULL)) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    const uint8_t total = (uint8_t)(input_count + output_count);
    if (total == 0 || total > out_max) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    uint8_t n = 0;
    for (uint8_t i = 0; i < input_count; i++) {
        out[n].cluster_id = input[i];
        out[n].role = HA_ZB_ROLE_SERVER;
        out[n].reserved = 0;
        n++;
    }
    for (uint8_t i = 0; i < output_count; i++) {
        out[n].cluster_id = output[i];
        out[n].role = HA_ZB_ROLE_CLIENT;
        out[n].reserved = 0;
        n++;
    }

    *out_count = n;
    return SYS_OK;
}

sys_error_t zigbee_interview_apply(domain_t *domain, const zigbee_interview_result_t *result)
{
    if (domain == NULL || result == NULL || result->endpoint_count > ZIGBEE_INTERVIEW_ENDPOINTS_MAX) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    bool changed = false;
    const sys_error_t device =
        zigbee_device_set_model(domain, result->uid, result->model, &changed);
    if (sys_failed(device)) {
        return device;
    }

    for (uint8_t i = 0; i < result->endpoint_count; i++) {
        const zigbee_interview_endpoint_t *endpoint = &result->endpoints[i];
        const zigbee_endpoint_desc_t desc = {
            .device_uid = result->uid,
            .endpoint = endpoint->endpoint,
            .profile_id = endpoint->profile_id,
            .device_id = endpoint->device_id,
            .cluster_count = endpoint->cluster_count,
            .clusters = endpoint->clusters,
        };
        const sys_error_t topology = zigbee_topology_apply(domain, &desc, &changed);
        if (sys_failed(topology)) {
            return topology;
        }
    }
    return SYS_OK;
}
