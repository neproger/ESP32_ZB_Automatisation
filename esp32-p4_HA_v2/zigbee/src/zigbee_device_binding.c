#include "zigbee/zigbee_device_binding.h"

bool zb_device_binding_find_device_id(const zb_device_binding_t *list, size_t count,
                                      ha_device_uid_t uid, ha_device_id_t *out)
{
    if (list == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (list[i].device_uid == uid) {
            *out = list[i].device_id;
            return true;
        }
    }
    return false;
}

bool zb_device_binding_find_uid(const zb_device_binding_t *list, size_t count,
                                ha_device_id_t device_id, ha_device_uid_t *out)
{
    if (list == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (list[i].device_id == device_id) {
            *out = list[i].device_uid;
            return true;
        }
    }
    return false;
}

bool zb_device_binding_add(zb_device_binding_t *list, size_t capacity, size_t *count,
                           ha_device_id_t device_id, ha_device_uid_t uid)
{
    if (list == NULL || count == NULL || device_id == HA_DEVICE_ID_NONE) {
        return false;
    }

    ha_device_id_t existing_id = HA_DEVICE_ID_NONE;
    if (zb_device_binding_find_device_id(list, *count, uid, &existing_id)) {
        return existing_id == device_id; /* идемпотентно / конфликт uid */
    }
    ha_device_uid_t existing_uid = 0;
    if (zb_device_binding_find_uid(list, *count, device_id, &existing_uid)) {
        return existing_uid == uid; /* идемпотентно / конфликт device_id */
    }
    if (*count >= capacity) {
        return false;
    }

    list[*count].device_id = device_id;
    list[*count].device_uid = uid;
    (*count)++;
    return true;
}

bool zb_device_binding_remove(zb_device_binding_t *list, size_t *count, ha_device_uid_t uid)
{
    if (list == NULL || count == NULL) {
        return false;
    }
    for (size_t i = 0; i < *count; i++) {
        if (list[i].device_uid == uid) {
            list[i] = list[*count - 1]; /* swap-remove: порядок не значим */
            (*count)--;
            return true;
        }
    }
    return false;
}
