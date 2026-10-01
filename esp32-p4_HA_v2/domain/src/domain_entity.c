#include "domain/domain.h"

#include "domain_internal.h"
#include "mstore/mstore_table.h"

static domain_entity_entry_t *entry_of(const domain_t *domain, domain_entity_t type)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL) {
        return NULL;
    }
    return domain_entry_find(state, type);
}

domain_err_t domain_entity_put(domain_t *domain, domain_entity_t type,
                               const void *key, const void *record, bool *out_changed)
{
    domain_entity_entry_t *entry = entry_of(domain, type);
    if (entry == NULL) {
        return DOMAIN_NOT_FOUND;
    }
    if (key == NULL || record == NULL || out_changed == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    *out_changed = false;

    mstore_slot_t slot = 0;
    mstore_err_t err = mstore_table_slot_find(&entry->table, key, &slot);
    if (err == MSTORE_NOT_FOUND) {
        uint32_t generation = 0;
        err = mstore_table_slot_allocate(&entry->table, key, record, &slot, &generation);
        if (err != MSTORE_OK) {
            return domain_err_from_mstore(err);
        }
        *out_changed = true;
        return DOMAIN_OK;
    }
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    mstore_meta_t meta = {0};
    err = mstore_table_slot_meta(&entry->table, slot, &meta);
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    bool changed = false;
    err = mstore_table_slot_update(&entry->table, slot, meta.generation, record, &changed);
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    *out_changed = changed;
    return DOMAIN_OK;
}

domain_err_t domain_entity_get(domain_t *domain, domain_entity_t type,
                               const void *key, void *out_record)
{
    domain_entity_entry_t *entry = entry_of(domain, type);
    if (entry == NULL) {
        return DOMAIN_NOT_FOUND;
    }
    if (key == NULL || out_record == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    mstore_slot_t slot = 0;
    const mstore_err_t err = mstore_table_slot_find(&entry->table, key, &slot);
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    mstore_meta_t meta = {0};
    return domain_err_from_mstore(
        mstore_table_slot_read(&entry->table, slot, &meta, entry->scratch_key, out_record));
}

domain_err_t domain_entity_remove(domain_t *domain, domain_entity_t type, const void *key)
{
    domain_entity_entry_t *entry = entry_of(domain, type);
    if (entry == NULL) {
        return DOMAIN_NOT_FOUND;
    }
    if (key == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    mstore_slot_t slot = 0;
    mstore_err_t err = mstore_table_slot_find(&entry->table, key, &slot);
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    mstore_meta_t meta = {0};
    err = mstore_table_slot_meta(&entry->table, slot, &meta);
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    return domain_err_from_mstore(mstore_table_slot_free(&entry->table, slot, meta.generation));
}

typedef struct {
    domain_entity_iter_cb_t cb;
    void *ctx;
} iter_bridge_t;

static bool iter_bridge(mstore_slot_t slot, const mstore_meta_t *meta,
                        const void *key, const void *payload, void *ctx)
{
    (void)slot;
    (void)meta;
    const iter_bridge_t *bridge = (const iter_bridge_t *)ctx;
    return bridge->cb(key, payload, bridge->ctx);
}

domain_err_t domain_entity_iter(domain_t *domain, domain_entity_t type,
                                domain_entity_iter_cb_t cb, void *ctx)
{
    domain_entity_entry_t *entry = entry_of(domain, type);
    if (entry == NULL) {
        return DOMAIN_NOT_FOUND;
    }
    if (cb == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    iter_bridge_t bridge = {.cb = cb, .ctx = ctx};
    return domain_err_from_mstore(mstore_table_iter(&entry->table, iter_bridge, &bridge));
}
