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

/* Канонический native_id Zigbee-устройства: lowercase hex EUI64 (16), без 0x/: */
bool zb_device_native_id(ha_device_uid_t uid, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    const int written = snprintf(out, out_size, "%016llx", (unsigned long long)uid);
    return written > 0 && (size_t)written < out_size;
}

bool zb_uid_from_native_id(const char *native_id, ha_device_uid_t *out)
{
    if (native_id == NULL || out == NULL || native_id[0] == '\0') {
        return false;
    }
    ha_device_uid_t value = 0;
    for (const char *p = native_id; *p != '\0'; p++) {
        int digit;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            digit = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            digit = *p - 'A' + 10;
        } else {
            return false;
        }
        value = (value << 4) | (ha_device_uid_t)digit;
    }
    *out = value;
    return true;
}

bool zb_entity_binding_remove_uid(zb_entity_binding_t *list, size_t *count, ha_device_uid_t uid)
{
    if (list == NULL || count == NULL) {
        return false;
    }
    bool removed = false;
    size_t i = 0;
    while (i < *count) {
        if (list[i].device_uid == uid) {
            list[i] = list[*count - 1];
            (*count)--;
            removed = true;
        } else {
            i++;
        }
    }
    return removed;
}

/*
 * Детерминированный Logical Device id из EUI64: канонический native_id → единая generic
 * derivation ha_device_id_derive (источник — adapter, формула — одна).
 */
ha_device_id_t zb_device_id(ha_device_uid_t uid)
{
    char native_id[HA_NATIVE_ID_MAX];
    if (!zb_device_native_id(uid, native_id, sizeof(native_id))) {
        return HA_DEVICE_ID_NONE;
    }
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
        sys_failed(domain_entity_get(domain, (domain_entity_t)HA_ENTITY_ZB_DIAG_DEVICE, &uid, &device)) ||
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
                                    const char *initial_name, ha_entity_id_t *out_entity_id)
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
     * Adapter отвечает только за identity/существование Entity и initial name; ownership
     * (device_id) пишет Device service, поэтому здесь не трогается. Дозаполняем лишь пустое
     * имя; уже заданное не перетираем.
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
    } else {
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
