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
 * собирать запись Journal умеет только Domain (см. entity_op_write_fact).
 */
static void journal_append_fact(domain_state_t *state, domain_event_t *fact)
{
    domain_event_id_t id = 0;
    (void)domain_journal_append(&state->journal, fact, &id);
    domain_dispatch_signal(state);
}

/*
 * Контекст операции над одной записью. Все шаги операции (find/allocate/update/…)
 * работают с одним ключом и одним типом, поэтому факт собирается из этого контекста
 * одной строкой, а не повторяет type и key_size на каждом вызове mstore.
 *
 * Контекст не решает, что считать ошибкой: решение остаётся на месте вызова.
 */
typedef struct {
    domain_state_t *state;
    domain_entity_entry_t *entry;
    const void *key;
    uint8_t key_size;
    uint8_t op;
    const domain_fact_meta_t *meta;
} entity_op_t;

static entity_op_t entity_op_begin(domain_state_t *state, domain_entity_entry_t *entry,
                                   const void *key, uint8_t op,
                                   const domain_fact_meta_t *meta)
{
    /* Обходу ключа нет: key_size берётся из descriptor'а только когда ключ есть. */
    const entity_op_t op_ctx = {
        .state = state,
        .entry = entry,
        .key = key,
        .key_size = (key == NULL) ? 0 : (uint8_t)entry->desc.key_size,
        .op = op,
        .meta = meta,
    };
    return op_ctx;
}

static void entity_op_write_fact(const entity_op_t *op, uint8_t kind, uint32_t error)
{
    /*
     * Инварианты, а не проверки: key_size приходит из descriptor'а, а регистрация типа
     * не пропускает ключ шире DOMAIN_JOURNAL_KEY_MAX и source ≥ DOMAIN_SOURCE_ID_MAX.
     * Проверять это на каждом факте незачем — в release assert стоит ноль.
     */
    assert(op->key_size <= DOMAIN_EVENT_KEY_MAX);
    assert(op->meta == NULL || op->meta->source < DOMAIN_SOURCE_ID_MAX);

    domain_event_t fact = {0};
    fact.ts = domain_platform_now_ms();
    fact.kind = kind;
    fact.op = op->op;
    fact.key_size = op->key_size;
    fact.entity = op->entry->desc.type;
    fact.error = error;
    if (op->meta != NULL) {
        fact.source = op->meta->source;
        fact.value = op->meta->value;
        fact.payload_ref = op->meta->payload_ref;
    } else {
        fact.source = (uint8_t)DOMAIN_SOURCE_SYSTEM;
    }
    if (op->key != NULL && op->key_size > 0) {
        memcpy(fact.key, op->key, op->key_size);
    }
    journal_append_fact(op->state, &fact);
}

static void entity_op_state(const entity_op_t *op, uint8_t kind)
{
    entity_op_write_fact(op, kind, 0);
}

/*
 * Сбой операции: фиксируется фактом ERROR и возвращается вызывающему тем же кодом.
 * Journal не влияет на решение — решение принято на месте вызова.
 */
static domain_err_t entity_op_error(const entity_op_t *op, mstore_err_t err)
{
    const domain_err_t result = domain_err_from_mstore(err);
    entity_op_write_fact(op, (uint8_t)DOMAIN_FACT_ERROR, (uint32_t)result);
    return result;
}

static domain_err_t put_locked(domain_state_t *state, domain_entity_entry_t *entry,
                               const void *key, const void *record,
                               const domain_fact_meta_t *meta, bool *out_changed)
{
    *out_changed = false;

    const entity_op_t op =
        entity_op_begin(state, entry, key, (uint8_t)DOMAIN_OP_ENTITY_PUT, meta);

    bool changed = false;
    mstore_slot_t slot = 0;
    mstore_err_t err = mstore_table_slot_find(&entry->table, key, &slot);

    if (err == MSTORE_NOT_FOUND) {
        /* Ожидаемая ветка: записи ещё нет. */
        uint32_t generation = 0;
        err = mstore_table_slot_allocate(&entry->table, key, record, &slot, &generation);
        if (err != MSTORE_OK) {
            return entity_op_error(&op, err);
        }
        changed = true;
    } else if (err == MSTORE_OK) {
        mstore_meta_t slot_meta = {0};
        err = mstore_table_slot_meta(&entry->table, slot, &slot_meta);
        if (err != MSTORE_OK) {
            return entity_op_error(&op, err);
        }
        err = mstore_table_slot_update(&entry->table, slot, slot_meta.generation, record,
                                       &changed);
        if (err != MSTORE_OK) {
            return entity_op_error(&op, err);
        }
    } else {
        return entity_op_error(&op, err);
    }

    if (!changed) {
        return DOMAIN_OK;
    }

    entity_op_state(&op, (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED);
    *out_changed = changed;
    return DOMAIN_OK;
}

static domain_err_t get_locked(domain_state_t *state, domain_entity_entry_t *entry,
                               const void *key, void *out_record,
                               const domain_fact_meta_t *meta)
{
    const entity_op_t op =
        entity_op_begin(state, entry, key, (uint8_t)DOMAIN_OP_ENTITY_GET, meta);

    mstore_slot_t slot = 0;
    mstore_err_t err = mstore_table_slot_find(&entry->table, key, &slot);
    if (err != MSTORE_OK) {
        if (err == MSTORE_NOT_FOUND) {
            return DOMAIN_NOT_FOUND; /* читать нечего — норма */
        }
        return entity_op_error(&op, err);
    }

    mstore_meta_t slot_meta = {0};
    err = mstore_table_slot_read(&entry->table, slot, &slot_meta, entry->scratch_key,
                                 out_record);
    if (err != MSTORE_OK) {
        return entity_op_error(&op, err);
    }
    return DOMAIN_OK;
}

static domain_err_t remove_locked(domain_state_t *state, domain_entity_entry_t *entry,
                                  const void *key, const domain_fact_meta_t *meta)
{
    const entity_op_t op =
        entity_op_begin(state, entry, key, (uint8_t)DOMAIN_OP_ENTITY_REMOVE, meta);

    mstore_slot_t slot = 0;
    mstore_err_t err = mstore_table_slot_find(&entry->table, key, &slot);
    if (err != MSTORE_OK) {
        if (err == MSTORE_NOT_FOUND) {
            return DOMAIN_NOT_FOUND; /* удалять нечего — норма */
        }
        return entity_op_error(&op, err);
    }

    mstore_meta_t slot_meta = {0};
    err = mstore_table_slot_meta(&entry->table, slot, &slot_meta);
    if (err != MSTORE_OK) {
        return entity_op_error(&op, err);
    }

    err = mstore_table_slot_free(&entry->table, slot, slot_meta.generation);
    if (err != MSTORE_OK) {
        return entity_op_error(&op, err);
    }

    entity_op_state(&op, (uint8_t)DOMAIN_FACT_ENTITY_REMOVED);
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
    domain_platform_lock_release(state->lock);
    return result;
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
        (entry == NULL) ? DOMAIN_NOT_FOUND : get_locked(state, entry, key, out_record, NULL);
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
    domain_platform_lock_release(state->lock);
    return result;
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
    domain_err_t result = DOMAIN_NOT_FOUND;
    if (entry != NULL) {
        const entity_op_t op =
            entity_op_begin(state, entry, NULL, (uint8_t)DOMAIN_OP_ENTITY_ITER, NULL);
        const mstore_err_t err = mstore_table_iter(&entry->table, iter_bridge, &bridge);
        result = (err == MSTORE_OK) ? DOMAIN_OK : entity_op_error(&op, err);
    }
    domain_platform_lock_release(state->lock);
    return result;
}
