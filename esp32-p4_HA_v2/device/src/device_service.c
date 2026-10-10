#include "device/device_service.h"

#include <string.h>

#include "ha_model/ha_groups.h" /* ha_group_item_key_t */
#include "ha_model/ha_state.h"  /* ha_state_key_t */

#define DEVICE_REMOVERS_MAX 4

typedef struct {
    bool used;
    ha_source_id_t source;
    device_remover_fn fn;
    void *ctx;
} device_remover_slot_t;

static device_remover_slot_t s_removers[DEVICE_REMOVERS_MAX];

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

sys_error_t device_register_remover(ha_source_id_t source, device_remover_fn fn, void *ctx)
{
    if (fn == NULL || source == HA_SOURCE_NONE) {
        return device_fail(SYS_CODE_INVALID_ARG);
    }
    for (size_t i = 0; i < DEVICE_REMOVERS_MAX; i++) {
        if (s_removers[i].used && s_removers[i].source == source) {
            s_removers[i].fn = fn; /* идемпотентная перерегистрация */
            s_removers[i].ctx = ctx;
            return SYS_OK;
        }
    }
    for (size_t i = 0; i < DEVICE_REMOVERS_MAX; i++) {
        if (!s_removers[i].used) {
            s_removers[i] = (device_remover_slot_t){
                .used = true, .source = source, .fn = fn, .ctx = ctx};
            return SYS_OK;
        }
    }
    return device_fail(SYS_CODE_NO_SPACE);
}

static const device_remover_slot_t *find_remover(ha_source_id_t source)
{
    for (size_t i = 0; i < DEVICE_REMOVERS_MAX; i++) {
        if (s_removers[i].used && s_removers[i].source == source) {
            return &s_removers[i];
        }
    }
    return NULL;
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
        /* Имя — собственность Device: initial name заполняем только если пусто. */
        if (existing.name[0] == '\0' && desc->name[0] != '\0') {
            ha_logical_device_record_t rec = existing;
            copy_field(rec.name, sizeof(rec.name), desc->name);
            domain_fact_meta_t meta = {0};
            meta.source = origin;
            bool changed = false;
            (void)domain_entity_put(domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE, &key, &rec,
                                    &meta, &changed);
        }
        *out_device_id = device_id;
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
    if (record.device_id != HA_DEVICE_ID_NONE) {
        return device_fail(SYS_CODE_INVALID_STATE); /* reassign пока не поддержан */
    }

    record.device_id = device_id;
    domain_fact_meta_t meta = {0};
    meta.source = origin;
    bool changed = false;
    return domain_entity_put(domain, (domain_entity_t)HA_ENTITY_ENTITY, &key, &record, &meta,
                             &changed);
}

sys_error_t device_rename(domain_t *domain, uint8_t origin, ha_device_id_t device_id,
                          const char *name)
{
    if (domain == NULL || name == NULL || device_id == HA_DEVICE_ID_NONE) {
        return device_fail(SYS_CODE_INVALID_ARG);
    }
    const ha_logical_device_key_t key = {.id = device_id};
    ha_logical_device_record_t record = {0};
    const sys_error_t found = domain_entity_get(domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE,
                                               &key, &record);
    if (sys_failed(found)) {
        return found;
    }
    copy_field(record.name, sizeof(record.name), name);

    domain_fact_meta_t meta = {0};
    meta.source = origin;
    bool changed = false;
    return domain_entity_put(domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE, &key, &record,
                             &meta, &changed);
}

/* --- cascade cleanup ----------------------------------------------------- */

typedef struct {
    ha_device_id_t device_id;
    ha_entity_key_t found;
    bool has;
} owned_ctx_t;

static bool owned_match(const void *key, const void *record, void *ctx)
{
    const ha_entity_record_t *rec = (const ha_entity_record_t *)record;
    owned_ctx_t *find = (owned_ctx_t *)ctx;
    if (rec->device_id == find->device_id) {
        memcpy(&find->found, key, sizeof(find->found));
        find->has = true;
        return false;
    }
    return true;
}

typedef struct {
    ha_entity_id_t entity;
    ha_state_key_t found;
    bool has;
} ps_ctx_t;

static bool ps_match(const void *key, const void *record, void *ctx)
{
    (void)record;
    const ha_state_key_t *k = (const ha_state_key_t *)key;
    ps_ctx_t *find = (ps_ctx_t *)ctx;
    if (k->entity == find->entity) {
        find->found = *k;
        find->has = true;
        return false;
    }
    return true;
}

typedef struct {
    ha_entity_id_t entity;
    ha_group_item_key_t found;
    bool has;
} gi_ctx_t;

static bool gi_match(const void *key, const void *record, void *ctx)
{
    (void)record;
    const ha_group_item_key_t *k = (const ha_group_item_key_t *)key;
    gi_ctx_t *find = (gi_ctx_t *)ctx;
    if (k->state.entity == find->entity) {
        find->found = *k;
        find->has = true;
        return false;
    }
    return true;
}

/* Удалить canonical-следы Entity: property state, capabilities, ссылки group item, саму Entity. */
static sys_error_t cascade_entity(domain_t *domain, uint8_t origin, ha_entity_id_t entity_id)
{
    domain_fact_meta_t meta = {0};
    meta.source = origin;

    for (;;) {
        ps_ctx_t find = {.entity = entity_id, .has = false};
        const sys_error_t it = domain_entity_iter(domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE,
                                                  ps_match, &find);
        if (sys_failed(it)) {
            return it;
        }
        if (!find.has) {
            break;
        }
        const sys_error_t err =
            domain_entity_remove(domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &find.found,
                                 &meta);
        if (sys_failed(err) && !sys_is(err, SYS_CODE_NOT_FOUND)) {
            return err;
        }
    }

    for (;;) {
        gi_ctx_t find = {.entity = entity_id, .has = false};
        const sys_error_t it =
            domain_entity_iter(domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, gi_match, &find);
        if (sys_failed(it)) {
            return it;
        }
        if (!find.has) {
            break;
        }
        const sys_error_t err = domain_entity_remove(
            domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &find.found, &meta);
        if (sys_failed(err) && !sys_is(err, SYS_CODE_NOT_FOUND)) {
            return err;
        }
    }

    const ha_entity_key_t key = {.id = entity_id};
    (void)domain_entity_remove(domain, (domain_entity_t)HA_ENTITY_CAPABILITIES, &key, &meta);
    const sys_error_t err =
        domain_entity_remove(domain, (domain_entity_t)HA_ENTITY_ENTITY, &key, &meta);
    if (sys_failed(err) && !sys_is(err, SYS_CODE_NOT_FOUND)) {
        return err;
    }
    return SYS_OK;
}

sys_error_t device_remove(domain_t *domain, uint8_t origin, ha_device_id_t device_id)
{
    if (domain == NULL || device_id == HA_DEVICE_ID_NONE) {
        return device_fail(SYS_CODE_INVALID_ARG);
    }

    /* 1. Прочитать provenance и убедиться, что Device существует. */
    const ha_logical_device_key_t key = {.id = device_id};
    ha_logical_device_record_t record = {0};
    const sys_error_t found = domain_entity_get(domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE,
                                               &key, &record);
    if (sys_failed(found)) {
        return found; /* NOT_FOUND */
    }

    /* 2. Транспортное forget/remove (adapter). Ошибка → canonical state не трогаем. */
    const device_remover_slot_t *remover = find_remover(record.source);
    if (remover == NULL) {
        return device_fail(SYS_CODE_INVALID_STATE);
    }
    const sys_error_t removed = remover->fn(record.native_id, remover->ctx);
    if (sys_failed(removed)) {
        return removed;
    }

    /* 3. Cascade: canonical-следы всех Entity этого Device, затем сам Device. */
    for (;;) {
        owned_ctx_t find = {.device_id = device_id, .has = false};
        const sys_error_t it =
            domain_entity_iter(domain, (domain_entity_t)HA_ENTITY_ENTITY, owned_match, &find);
        if (sys_failed(it)) {
            return it;
        }
        if (!find.has) {
            break;
        }
        const sys_error_t err = cascade_entity(domain, origin, find.found.id);
        if (sys_failed(err)) {
            return err;
        }
    }

    domain_fact_meta_t meta = {0};
    meta.source = origin;
    const sys_error_t err =
        domain_entity_remove(domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE, &key, &meta);
    if (sys_failed(err) && !sys_is(err, SYS_CODE_NOT_FOUND)) {
        return err;
    }
    return SYS_OK;
}
