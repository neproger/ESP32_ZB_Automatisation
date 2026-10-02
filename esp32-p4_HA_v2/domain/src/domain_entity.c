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

static void entity_op_write_fact(const entity_op_t *op, uint8_t kind, sys_error_t error)
{
    domain_fact_write(op->state, op->meta, op->entry->desc.type, op->key, op->key_size, kind,
                      op->op, error, NULL);
}

static void entity_op_state(const entity_op_t *op, uint8_t kind)
{
    entity_op_write_fact(op, kind, SYS_OK);
}

/*
 * Сбой операции: фиксируется фактом ERROR и возвращается вызывающему той же ошибкой.
 * Journal не влияет на решение — решение принято на месте вызова.
 */
static sys_error_t entity_op_error(const entity_op_t *op, sys_error_t err)
{
    entity_op_write_fact(op, (uint8_t)DOMAIN_FACT_ERROR, err);
    return err;
}

static sys_error_t put_locked(domain_state_t *state, domain_entity_entry_t *entry,
                               const void *key, const void *record,
                               const domain_fact_meta_t *meta, bool *out_changed)
{
    *out_changed = false;

    const entity_op_t op =
        entity_op_begin(state, entry, key, (uint8_t)DOMAIN_OP_ENTITY_PUT, meta);

    bool changed = false;
    mstore_slot_t slot = 0;
    sys_error_t err = mstore_table_slot_find(&entry->table, key, &slot);

    if (sys_is(err, SYS_CODE_NOT_FOUND)) {
        /* Ожидаемая ветка: записи ещё нет. */
        uint32_t generation = 0;
        err = mstore_table_slot_allocate(&entry->table, key, record, &slot, &generation);
        if (sys_failed(err)) {
            return entity_op_error(&op, err);
        }
        changed = true;
    } else if (sys_ok(err)) {
        mstore_meta_t slot_meta = {0};
        err = mstore_table_slot_meta(&entry->table, slot, &slot_meta);
        if (sys_failed(err)) {
            return entity_op_error(&op, err);
        }
        err = mstore_table_slot_update(&entry->table, slot, slot_meta.generation, record,
                                       &changed);
        if (sys_failed(err)) {
            return entity_op_error(&op, err);
        }
    } else {
        return entity_op_error(&op, err);
    }

    if (!changed) {
        return SYS_OK;
    }

    entity_op_state(&op, (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED);
    *out_changed = changed;
    return SYS_OK;
}

static sys_error_t get_locked(domain_state_t *state, domain_entity_entry_t *entry,
                               const void *key, void *out_record,
                               const domain_fact_meta_t *meta)
{
    const entity_op_t op =
        entity_op_begin(state, entry, key, (uint8_t)DOMAIN_OP_ENTITY_GET, meta);

    mstore_slot_t slot = 0;
    sys_error_t err = mstore_table_slot_find(&entry->table, key, &slot);
    if (sys_failed(err)) {
        if (sys_is(err, SYS_CODE_NOT_FOUND)) {
            return domain_fail(SYS_CODE_NOT_FOUND); /* читать нечего — норма */
        }
        return entity_op_error(&op, err);
    }

    mstore_meta_t slot_meta = {0};
    err = mstore_table_slot_read(&entry->table, slot, &slot_meta, entry->scratch_key,
                                 out_record);
    if (sys_failed(err)) {
        return entity_op_error(&op, err);
    }
    return SYS_OK;
}

static sys_error_t remove_locked(domain_state_t *state, domain_entity_entry_t *entry,
                                  const void *key, const domain_fact_meta_t *meta)
{
    const entity_op_t op =
        entity_op_begin(state, entry, key, (uint8_t)DOMAIN_OP_ENTITY_REMOVE, meta);

    mstore_slot_t slot = 0;
    sys_error_t err = mstore_table_slot_find(&entry->table, key, &slot);
    if (sys_failed(err)) {
        if (sys_is(err, SYS_CODE_NOT_FOUND)) {
            return domain_fail(SYS_CODE_NOT_FOUND); /* удалять нечего — норма */
        }
        return entity_op_error(&op, err);
    }

    mstore_meta_t slot_meta = {0};
    err = mstore_table_slot_meta(&entry->table, slot, &slot_meta);
    if (sys_failed(err)) {
        return entity_op_error(&op, err);
    }

    err = mstore_table_slot_free(&entry->table, slot, slot_meta.generation);
    if (sys_failed(err)) {
        return entity_op_error(&op, err);
    }

    entity_op_state(&op, (uint8_t)DOMAIN_FACT_ENTITY_REMOVED);
    return SYS_OK;
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

sys_error_t domain_entity_put(domain_t *domain, domain_entity_t type,
                               const void *key, const void *record,
                               const domain_fact_meta_t *meta, bool *out_changed)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || key == NULL || record == NULL || out_changed == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    domain_platform_lock_acquire(state->lock);
    domain_entity_entry_t *entry = domain_entry_find(state, type);
    const sys_error_t result = (entry == NULL)
                                    ? domain_fail(SYS_CODE_NOT_FOUND)
                                    : put_locked(state, entry, key, record, meta, out_changed);
    domain_platform_lock_release(state->lock);
    return result;
}

sys_error_t domain_entity_get(domain_t *domain, domain_entity_t type,
                               const void *key, void *out_record)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || key == NULL || out_record == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    domain_platform_lock_acquire(state->lock);
    domain_entity_entry_t *entry = domain_entry_find(state, type);
    const sys_error_t result =
        (entry == NULL) ? domain_fail(SYS_CODE_NOT_FOUND) : get_locked(state, entry, key, out_record, NULL);
    domain_platform_lock_release(state->lock);
    return result;
}

sys_error_t domain_entity_remove(domain_t *domain, domain_entity_t type, const void *key,
                                  const domain_fact_meta_t *meta)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || key == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    domain_platform_lock_acquire(state->lock);
    domain_entity_entry_t *entry = domain_entry_find(state, type);
    const sys_error_t result =
        (entry == NULL) ? domain_fail(SYS_CODE_NOT_FOUND) : remove_locked(state, entry, key, meta);
    domain_platform_lock_release(state->lock);
    return result;
}

sys_error_t domain_entity_iter(domain_t *domain, domain_entity_t type,
                                domain_entity_iter_cb_t cb, void *ctx)
{
    domain_state_t *state = domain_state(domain);
    if (state == NULL || cb == NULL) {
        return domain_fail(SYS_CODE_INVALID_ARG);
    }

    iter_bridge_t bridge = {.cb = cb, .ctx = ctx};
    domain_platform_lock_acquire(state->lock);
    domain_entity_entry_t *entry = domain_entry_find(state, type);
    sys_error_t result = domain_fail(SYS_CODE_NOT_FOUND);
    if (entry != NULL) {
        const entity_op_t op =
            entity_op_begin(state, entry, NULL, (uint8_t)DOMAIN_OP_ENTITY_ITER, NULL);
        const sys_error_t err = mstore_table_iter(&entry->table, iter_bridge, &bridge);
        result = (sys_ok(err)) ? SYS_OK : entity_op_error(&op, err);
    }
    domain_platform_lock_release(state->lock);
    return result;
}
