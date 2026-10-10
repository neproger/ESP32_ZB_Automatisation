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

bool zb_entity_seed(ha_device_uid_t uid, uint8_t endpoint, uint8_t out[9])
{
    if (out == NULL) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(uid >> (8 * i)); /* LE, без memcpy(struct) */
    }
    out[8] = endpoint;
    return true;
}

bool zb_entity_binding_add(zb_entity_binding_t *list, size_t capacity, size_t *count,
                           ha_device_uid_t uid, uint8_t endpoint, ha_entity_id_t entity_id)
{
    if (list == NULL || count == NULL || *count >= capacity) {
        return false;
    }
    list[*count].entity_id = entity_id;
    list[*count].device_uid = uid;
    list[*count].endpoint = endpoint;
    (*count)++;
    return true;
}

bool zb_entity_resolve(zb_entity_binding_t *list, size_t capacity, size_t *count,
                       ha_device_uid_t uid, uint8_t endpoint, ha_entity_id_t *out_entity_id,
                       bool *out_created)
{
    if (list == NULL || count == NULL || out_entity_id == NULL) {
        return false;
    }
    if (out_created != NULL) {
        *out_created = false;
    }

    ha_entity_id_t existing = HA_ENTITY_ID_NONE;
    if (zb_entity_binding_find_entity(list, *count, uid, endpoint, &existing)) {
        *out_entity_id = existing;
        return true; /* идемпотентно: тот же entity_id, новая не создаётся */
    }

    uint8_t seed[9];
    if (!zb_entity_seed(uid, endpoint, seed)) {
        return false;
    }
    const ha_entity_id_t derived = ha_entity_id_derive(HA_ENTITY_ISSUER_ZIGBEE, seed, sizeof(seed));
    if (derived == HA_ENTITY_ID_NONE) { /* ложная identity не создаётся */
        return false;
    }
    if (!zb_entity_binding_add(list, capacity, count, uid, endpoint, derived)) {
        return false; /* нет места */
    }
    *out_entity_id = derived;
    if (out_created != NULL) {
        *out_created = true;
    }
    return true;
}
