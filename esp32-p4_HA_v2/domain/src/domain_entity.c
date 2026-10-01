#include "domain/domain.h"

#include <assert.h>
#include <string.h>

#include "domain_internal.h"
#include "domain_platform.h"
#include "mstore/mstore_table.h"

/*
 * Каждая операция целиком идёт под lock'ом уровня Domain:
 *   - mutation path: slot_find → slot_meta → slot_update/free должны быть одной
 *     серией, иначе слот успевают переиспользовать и получаем STALE;
 *   - get пишет ключ в scratch entry, поэтому read path тоже сериализован.
 *
 * Journal здесь — параллельный след операции, а не второй уровень проверки:
 * операция возвращает свой результат вызывающему, а факт пишется по_classify_
 * outcome (ERROR / STATE_CHANGED). Отказ append'а не меняет результат операции.
 */

/*
 * domain_fact_meta_t описан в публичном header'е, но Journal — внутренняя подсистема:
 * собирать запись Journal умеет только Domain.
 */
static void fact_fill(domain_event_t *out, domain_entity_t type, const void *key,
                      uint8_t key_size, uint8_t kind, uint8_t op,
                      const domain_fact_meta_t *meta)
{
    /*
     * Инвариант, а не проверка: key_size приходит из descriptor'а, а регистрация типа
     * не пропускает ключ шире DOMAIN_JOURNAL_KEY_MAX. Проверять это на каждом факте
     * незачем — в release assert стоит ноль.
     */
    assert(key_size <= DOMAIN_EVENT_KEY_MAX);
    assert(meta == NULL || meta->source < DOMAIN_SOURCE_ID_MAX);

    memset(out, 0, sizeof(*out));
    out->ts = domain_platform_now_ms();
    out->kind = kind;
    out->op = op;
    out->key_size = key_size;
    out->entity = type;
    if (meta != NULL) {
        out->source = meta->source;
        out->value = meta->value;
        out->payload_ref = meta->payload_ref;
    } else {
        out->source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    }
    memcpy(out->key, key, key_size);
}

static void journal_append_fact(domain_state_t *state, domain_event_t *fact)
{
    domain_event_id_t id = 0;
    (void)domain_journal_append(&state->journal, fact, &id);
    domain_dispatch_signal(state);
}

/*
 * Классификация outcome операции. Возвращает тот же результат, что был:
 * Journal не влияет на решение, а только фиксирует значимый исход.
 */
static domain_err_t journal_outcome(domain_state_t *state, domain_entity_t type,
                                    const void *key, uint8_t key_size, uint8_t op,
                                    const domain_fact_meta_t *meta, domain_err_t outcome)
{
    if (!domain_outcome_is_runtime_error(outcome)) {
        return outcome;
    }

    domain_event_t fact = {0};
    fact_fill(&fact, type, key, key_size, (uint8_t)DOMAIN_FACT_ERROR, op, meta);
    fact.error = (uint32_t)outcome;
    journal_append_fact(state, &fact);
    return outcome;
}

static domain_err_t put_locked(domain_state_t *state, domain_entity_entry_t *entry,
                               const void *key, const void *record,
                               const domain_fact_meta_t *meta, bool *out_changed)
{
    *out_changed = false;

    bool changed = false;
    mstore_slot_t slot = 0;
    mstore_err_t err = mstore_table_slot_find(&entry->table, key, &slot);

    if (err == MSTORE_NOT_FOUND) {
        uint32_t generation = 0;
        err = mstore_table_slot_allocate(&entry->table, key, record, &slot, &generation);
        if (err != MSTORE_OK) {
            return domain_err_from_mstore(err);
        }
        changed = true;
    } else if (err == MSTORE_OK) {
        mstore_meta_t slot_meta = {0};
        err = mstore_table_slot_meta(&entry->table, slot, &slot_meta);
        if (err != MSTORE_OK) {
            return domain_err_from_mstore(err);
        }
        err = mstore_table_slot_update(&entry->table, slot, slot_meta.generation, record,
                                       &changed);
        if (err != MSTORE_OK) {
            return domain_err_from_mstore(err);
        }
    } else {
        return domain_err_from_mstore(err);
    }

    if (!changed) {
        return DOMAIN_OK;
    }

    domain_event_t fact = {0};
    fact_fill(&fact, entry->desc.type, key, (uint8_t)entry->desc.key_size,
              (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED, (uint8_t)DOMAIN_OP_ENTITY_PUT, meta);
    journal_append_fact(state, &fact);

    *out_changed = changed;
    return DOMAIN_OK;
}

static domain_err_t get_locked(domain_entity_entry_t *entry, const void *key, void *out_record)
{
    mstore_slot_t slot = 0;
    const mstore_err_t err = mstore_table_slot_find(&entry->table, key, &slot);
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    mstore_meta_t meta = {0};
    return domain_err_from_mstore(
        mstore_table_slot_read(&entry->table, slot, &meta, entry->scratch_key, out_record));
}

static domain_err_t remove_locked(domain_state_t *state, domain_entity_entry_t *entry,
                                  const void *key, const domain_fact_meta_t *meta)
{
    mstore_slot_t slot = 0;
    mstore_err_t err = mstore_table_slot_find(&entry->table, key, &slot);
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    mstore_meta_t slot_meta = {0};
    err = mstore_table_slot_meta(&entry->table, slot, &slot_meta);
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    err = mstore_table_slot_free(&entry->table, slot, slot_meta.generation);
    if (err != MSTORE_OK) {
        return domain_err_from_mstore(err);
    }

    domain_event_t fact = {0};
    fact_fill(&fact, entry->desc.type, key, (uint8_t)entry->desc.key_size,
              (uint8_t)DOMAIN_FACT_ENTITY_REMOVED, (uint8_t)DOMAIN_OP_ENTITY_REMOVE, meta);
    journal_append_fact(state, &fact);
    return DOMAIN_OK;
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

domain_err_t domain_entity_put(domain_t *domain, domain_entity_t type,
                               const void *key, const void *record,
                               const domain_fact_meta_t *meta, bool *out_changed)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || key == NULL || record == NULL || out_changed == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    domain_platform_lock_acquire(state->lock);
    domain_entity_entry_t *entry = domain_entry_find(state, type);
    const domain_err_t result = (entry == NULL)
                                    ? DOMAIN_NOT_FOUND
                                    : put_locked(state, entry, key, record, meta, out_changed);
    const domain_err_t journaled =
        (entry == NULL)
            ? result
            : journal_outcome(state, type, key, (uint8_t)entry->desc.key_size,
                              (uint8_t)DOMAIN_OP_ENTITY_PUT, meta, result);
    domain_platform_lock_release(state->lock);
    return journaled;
}

domain_err_t domain_entity_get(domain_t *domain, domain_entity_t type,
                               const void *key, void *out_record)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || key == NULL || out_record == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    domain_platform_lock_acquire(state->lock);
    domain_entity_entry_t *entry = domain_entry_find(state, type);
    const domain_err_t result =
        (entry == NULL) ? DOMAIN_NOT_FOUND : get_locked(entry, key, out_record);
    domain_platform_lock_release(state->lock);
    return result;
}

domain_err_t domain_entity_remove(domain_t *domain, domain_entity_t type, const void *key,
                                  const domain_fact_meta_t *meta)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || key == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    domain_platform_lock_acquire(state->lock);
    domain_entity_entry_t *entry = domain_entry_find(state, type);
    const domain_err_t result =
        (entry == NULL) ? DOMAIN_NOT_FOUND : remove_locked(state, entry, key, meta);
    const domain_err_t journaled =
        (entry == NULL)
            ? result
            : journal_outcome(state, type, key, (uint8_t)entry->desc.key_size,
                              (uint8_t)DOMAIN_OP_ENTITY_REMOVE, meta, result);
    domain_platform_lock_release(state->lock);
    return journaled;
}

domain_err_t domain_entity_iter(domain_t *domain, domain_entity_t type,
                                domain_entity_iter_cb_t cb, void *ctx)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || cb == NULL) {
        return DOMAIN_INVALID_ARG;
    }

    iter_bridge_t bridge = {.cb = cb, .ctx = ctx};
    domain_platform_lock_acquire(state->lock);
    domain_entity_entry_t *entry = domain_entry_find(state, type);
    const domain_err_t result = (entry == NULL)
                                    ? DOMAIN_NOT_FOUND
                                    : domain_err_from_mstore(
                                          mstore_table_iter(&entry->table, iter_bridge, &bridge));
    domain_platform_lock_release(state->lock);
    return result;
}
