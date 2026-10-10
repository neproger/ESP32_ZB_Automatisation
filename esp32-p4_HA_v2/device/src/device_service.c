#include "device/device_service.h"

#include <string.h>

static sys_error_t device_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_DEVICE, code);
}

static void copy_field(char *dst, size_t dst_size, const char *src)
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

sys_error_t device_register(domain_t *domain, uint8_t origin, const ha_device_desc_t *desc,
                            ha_device_id_t *out_device_id)
{
    if (domain == NULL || desc == NULL || out_device_id == NULL) {
        return device_fail(SYS_CODE_INVALID_ARG);
    }
    const ha_device_id_t device_id = ha_device_id_derive(desc->source, desc->native_id);
    if (device_id == HA_DEVICE_ID_NONE) {
        return device_fail(SYS_CODE_INVALID_ARG);
    }
    ha_device_id_t parent_device_id = HA_DEVICE_ID_NONE;
    if (desc->parent_native_id[0] != '\0') {
        parent_device_id = ha_device_id_derive(desc->source, desc->parent_native_id);
        if (parent_device_id == HA_DEVICE_ID_NONE) {
            return device_fail(SYS_CODE_INVALID_ARG);
        }
    }

    const ha_logical_device_key_t key = {.id = device_id};
    ha_logical_device_record_t existing = {0};
    if (sys_ok(domain_entity_get(domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE, &key,
                                 &existing))) {
        *out_device_id = device_id; /* имя/связи — собственность Device, не перетираем */
        return SYS_OK;
    }

    ha_logical_device_record_t record = {0};
    record.parent_device_id = parent_device_id;
    record.source = desc->source;
    copy_field(record.native_id, sizeof(record.native_id), desc->native_id);
    copy_field(record.name, sizeof(record.name), desc->name);

    domain_fact_meta_t meta = {0};
    meta.source = origin;
    bool changed = false;
    const sys_error_t err = domain_entity_put(domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE,
                                              &key, &record, &meta, &changed);
    if (sys_failed(err)) {
        return err;
    }
    *out_device_id = device_id;
    return SYS_OK;
}

sys_error_t device_bind_entity(domain_t *domain, uint8_t origin, ha_device_id_t device_id,
                               ha_entity_id_t entity_id)
{
    if (domain == NULL) {
        return device_fail(SYS_CODE_INVALID_ARG);
    }
    if (device_id == HA_DEVICE_ID_NONE || entity_id == HA_ENTITY_ID_NONE) {
        return device_fail(SYS_CODE_INVALID_ARG);
    }

    const ha_entity_key_t key = {.id = entity_id};
    ha_entity_record_t record = {0};
    const sys_error_t found = domain_entity_get(domain, (domain_entity_t)HA_ENTITY_ENTITY, &key,
                                               &record);
    if (sys_failed(found)) {
        return found; /* Entity ещё нет — владелец не привязывается */
    }
    if (record.device_id == device_id) {
        return SYS_OK; /* идемпотентно */
    }

    record.device_id = device_id;
    domain_fact_meta_t meta = {0};
    meta.source = origin;
    bool changed = false;
    return domain_entity_put(domain, (domain_entity_t)HA_ENTITY_ENTITY, &key, &record, &meta,
                             &changed);
}

typedef struct {
    ha_device_id_t device_id;
    ha_entity_key_t found;
    bool has;
} find_owned_ctx_t;

static bool find_owned(const void *key, const void *record, void *ctx)
{
    const ha_entity_record_t *rec = (const ha_entity_record_t *)record;
    find_owned_ctx_t *find = (find_owned_ctx_t *)ctx;
    if (rec->device_id == find->device_id) {
        memcpy(&find->found, key, sizeof(find->found));
        find->has = true;
        return false; /* нашли — прерываем */
    }
    return true;
}

sys_error_t device_remove(domain_t *domain, uint8_t origin, ha_device_id_t device_id)
{
    if (domain == NULL) {
        return device_fail(SYS_CODE_INVALID_ARG);
    }

    /* Мутировать Domain внутри iter нельзя — ищем по одному и снимаем ownership. */
    for (;;) {
        find_owned_ctx_t find = {.device_id = device_id, .has = false};
        const sys_error_t iter =
            domain_entity_iter(domain, (domain_entity_t)HA_ENTITY_ENTITY, find_owned, &find);
        if (sys_failed(iter)) {
            return iter;
        }
        if (!find.has) {
            break;
        }

        ha_entity_record_t record = {0};
        if (sys_failed(domain_entity_get(domain, (domain_entity_t)HA_ENTITY_ENTITY, &find.found,
                                         &record))) {
            continue; /* исчезла между проходами */
        }
        record.device_id = HA_DEVICE_ID_NONE;
        domain_fact_meta_t meta = {0};
        meta.source = origin;
        bool changed = false;
        const sys_error_t err = domain_entity_put(domain, (domain_entity_t)HA_ENTITY_ENTITY,
                                                  &find.found, &record, &meta, &changed);
        if (sys_failed(err)) {
            return err;
        }
    }

    const ha_logical_device_key_t key = {.id = device_id};
    domain_fact_meta_t meta = {0};
    meta.source = origin;
    const sys_error_t removed =
        domain_entity_remove(domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE, &key, &meta);
    if (sys_failed(removed) && !sys_is(removed, SYS_CODE_NOT_FOUND)) {
        return removed;
    }
    return SYS_OK;
}
