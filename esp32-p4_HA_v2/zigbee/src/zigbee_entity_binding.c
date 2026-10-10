#include "zigbee/zigbee_entity_binding.h"

bool zb_entity_binding_find_entity(const zb_entity_binding_t *list, size_t count,
                                   ha_device_uid_t uid, uint8_t endpoint, ha_entity_id_t *out)
{
    if (list == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (list[i].device_uid == uid && list[i].endpoint == endpoint) {
            *out = list[i].entity_id;
            return true;
        }
    }
    return false;
}

bool zb_entity_binding_find_physical(const zb_entity_binding_t *list, size_t count,
                                     ha_entity_id_t entity_id, ha_device_uid_t *uid, uint8_t *endpoint)
{
    if (list == NULL || uid == NULL || endpoint == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (list[i].entity_id == entity_id) {
            *uid = list[i].device_uid;
            *endpoint = list[i].endpoint;
            return true;
        }
    }
    return false;
}
