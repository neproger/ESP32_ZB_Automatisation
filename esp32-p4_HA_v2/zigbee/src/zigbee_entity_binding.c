#include "zigbee/zigbee_entity_binding.h"

#include <stdio.h>
#include <string.h>

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

/*
 * Детерминированный Logical Device id из EUI64. Канонический native_id: lowercase hex (16),
 * затем единая generic derivation ha_device_id_derive (источник — adapter, формула — одна).
 */
ha_device_id_t zb_device_id(ha_device_uid_t uid)
{
    char native_id[HA_NATIVE_ID_MAX];
    snprintf(native_id, sizeof(native_id), "%016llx", (unsigned long long)uid);
    return ha_device_id_derive(HA_SOURCE_ZIGBEE, native_id);
}

void zb_entity_initial_name(domain_t *domain, ha_device_uid_t uid, uint8_t endpoint,
                            uint8_t endpoint_total, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    ha_device_record_t device = {0};
    if (domain == NULL ||
        sys_failed(domain_entity_get(domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &device)) ||
        device.name[0] == '\0') {
        return;
    }
    if (endpoint_total <= 1) {
        snprintf(out, out_size, "%s", device.name);
    } else {
        snprintf(out, out_size, "%s - %u", device.name, (unsigned)endpoint);
    }
}

static void copy_entity_name(char *dst, size_t dst_size, const char *src)
{
    if (src == NULL) {
        return;
    }
    size_t length = strlen(src);
    if (length >= dst_size) {
        length = dst_size - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

sys_error_t zb_entity_ensure_entity(domain_t *domain, zb_entity_binding_t *list, size_t capacity,
                                    size_t *count, ha_device_uid_t uid, uint8_t endpoint,
                                    ha_device_id_t device_id, const char *initial_name,
                                    ha_entity_id_t *out_entity_id)
{
    if (domain == NULL || list == NULL || count == NULL || out_entity_id == NULL) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_ARG);
    }
    ha_entity_id_t id = HA_ENTITY_ID_NONE;
    bool created = false;
    if (!zb_entity_resolve(list, capacity, count, uid, endpoint, &id, &created)) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_NO_MEM);
    }

    const ha_entity_key_t key = {.id = id};

    /*
     * Имя и device_id — собственность Entity. Дозаполняем только отсутствующее: пустое имя
     * (adapter даёт initial name) и незаданный device_id (ownership). Уже заданное не трогаем.
     */
    ha_entity_record_t record = {0};
    bool need_put = true;
    ha_entity_record_t existing = {0};
    if (sys_ok(domain_entity_get(domain, (domain_entity_t)HA_ENTITY_ENTITY, &key, &existing))) {
        record = existing;
        need_put = false;
        if (record.name[0] == '\0' && initial_name != NULL) {
            copy_entity_name(record.name, sizeof(record.name), initial_name);
            need_put = true;
        }
        if (record.device_id == HA_DEVICE_ID_NONE && device_id != HA_DEVICE_ID_NONE) {
            record.device_id = device_id;
            need_put = true;
        }
    } else {
        record.device_id = device_id;
        copy_entity_name(record.name, sizeof(record.name), initial_name);
    }

    if (!need_put) {
        *out_entity_id = id;
        return SYS_OK;
    }

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    bool changed = false;
    const sys_error_t err = domain_entity_put(domain, (domain_entity_t)HA_ENTITY_ENTITY, &key,
                                              &record, &meta, &changed);
    if (sys_failed(err)) {
        if (created) {
            (*count)--; /* откат: привязка без Entity не остаётся */
        }
        return err;
    }
    *out_entity_id = id;
    return SYS_OK;
}
