#include "zigbee/zigbee_device_store.h"

#include <string.h>

#define ZB_DEVICE_STORE_MAX 32
#define ZB_ENDPOINT_STORE_MAX 64

typedef struct {
    bool used;
    ha_device_uid_t uid;
    ha_device_record_t record;
} zb_device_slot_t;

typedef struct {
    bool used;
    ha_device_uid_t uid;
    uint8_t endpoint;
    ha_endpoint_record_t record;
} zb_endpoint_slot_t;

static zb_device_slot_t s_devices[ZB_DEVICE_STORE_MAX];
static zb_endpoint_slot_t s_endpoints[ZB_ENDPOINT_STORE_MAX];

static sys_error_t zigbee_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_ZIGBEE, code);
}

static zb_device_slot_t *device_find(ha_device_uid_t uid)
{
    for (size_t i = 0; i < ZB_DEVICE_STORE_MAX; i++) {
        if (s_devices[i].used && s_devices[i].uid == uid) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static zb_device_slot_t *device_alloc(ha_device_uid_t uid)
{
    for (size_t i = 0; i < ZB_DEVICE_STORE_MAX; i++) {
        if (!s_devices[i].used) {
            s_devices[i].used = true;
            s_devices[i].uid = uid;
            memset(&s_devices[i].record, 0, sizeof(s_devices[i].record));
            return &s_devices[i];
        }
    }
    return NULL;
}

sys_error_t zigbee_device_ensure(ha_device_uid_t uid, bool *out_changed)
{
    if (out_changed == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    *out_changed = false;
    if (device_find(uid) != NULL) {
        return SYS_OK;
    }
    if (device_alloc(uid) == NULL) {
        return zigbee_fail(SYS_CODE_NO_MEM);
    }
    *out_changed = true;
    return SYS_OK;
}

sys_error_t zigbee_device_set_model(ha_device_uid_t uid, const char *model, bool *out_changed)
{
    if (model == NULL || out_changed == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    *out_changed = false;

    zb_device_slot_t *slot = device_find(uid);
    if (slot == NULL) {
        slot = device_alloc(uid);
        if (slot == NULL) {
            return zigbee_fail(SYS_CODE_NO_MEM);
        }
        *out_changed = true;
    }

    char trimmed[HA_DEVICE_MODEL_MAX] = {0};
    size_t length = strlen(model);
    if (length >= sizeof(trimmed)) {
        length = sizeof(trimmed) - 1;
    }
    memcpy(trimmed, model, length);

    if (strncmp(slot->record.model, trimmed, sizeof(trimmed)) != 0) {
        memcpy(slot->record.model, trimmed, sizeof(trimmed));
        *out_changed = true;
    }
    return SYS_OK;
}

bool zigbee_device_get(ha_device_uid_t uid, ha_device_record_t *out)
{
    if (out == NULL) {
        return false;
    }
    const zb_device_slot_t *slot = device_find(uid);
    if (slot == NULL) {
        return false;
    }
    *out = slot->record;
    return true;
}

bool zigbee_device_has(ha_device_uid_t uid)
{
    return device_find(uid) != NULL;
}

static zb_endpoint_slot_t *endpoint_find(ha_device_uid_t uid, uint8_t endpoint)
{
    for (size_t i = 0; i < ZB_ENDPOINT_STORE_MAX; i++) {
        if (s_endpoints[i].used && s_endpoints[i].uid == uid &&
            s_endpoints[i].endpoint == endpoint) {
            return &s_endpoints[i];
        }
    }
    return NULL;
}

sys_error_t zigbee_topology_apply(const zigbee_endpoint_desc_t *desc, bool *out_changed)
{
    if (desc == NULL || out_changed == NULL || desc->clusters == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (desc->cluster_count == 0 || desc->cluster_count > HA_ENDPOINT_CLUSTERS_MAX) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    *out_changed = false;

    if (device_find(desc->device_uid) == NULL) {
        if (device_alloc(desc->device_uid) == NULL) {
            return zigbee_fail(SYS_CODE_NO_MEM);
        }
        *out_changed = true;
    }

    ha_endpoint_record_t record = {0};
    record.profile_id = desc->profile_id;
    record.device_id = desc->device_id;
    record.cluster_count = desc->cluster_count;
    memcpy(record.clusters, desc->clusters, desc->cluster_count * sizeof(ha_cluster_entry_t));

    zb_endpoint_slot_t *slot = endpoint_find(desc->device_uid, desc->endpoint);
    if (slot == NULL) {
        for (size_t i = 0; i < ZB_ENDPOINT_STORE_MAX; i++) {
            if (!s_endpoints[i].used) {
                slot = &s_endpoints[i];
                slot->used = true;
                slot->uid = desc->device_uid;
                slot->endpoint = desc->endpoint;
                break;
            }
        }
        if (slot == NULL) {
            return zigbee_fail(SYS_CODE_NO_MEM);
        }
        slot->record = record;
        *out_changed = true;
        return SYS_OK;
    }

    if (memcmp(&slot->record, &record, sizeof(record)) != 0) {
        slot->record = record;
        *out_changed = true;
    }
    return SYS_OK;
}

bool zigbee_topology_get(ha_device_uid_t uid, uint8_t endpoint, ha_endpoint_record_t *out)
{
    if (out == NULL) {
        return false;
    }
    const zb_endpoint_slot_t *slot = endpoint_find(uid, endpoint);
    if (slot == NULL) {
        return false;
    }
    *out = slot->record;
    return true;
}

sys_error_t zigbee_device_remove(ha_device_uid_t uid)
{
    zb_device_slot_t *slot = device_find(uid);
    if (slot != NULL) {
        slot->used = false;
    }
    for (size_t i = 0; i < ZB_ENDPOINT_STORE_MAX; i++) {
        if (s_endpoints[i].used && s_endpoints[i].uid == uid) {
            s_endpoints[i].used = false;
        }
    }
    return SYS_OK;
}
