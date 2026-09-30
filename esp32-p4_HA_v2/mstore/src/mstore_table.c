#include <string.h>

#include "mstore/mstore_table.h"
#include "mstore_internal.h"

static size_t mstore_align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static size_t mstore_next_pow2(size_t value) {
    size_t result = 8;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

static bool mstore_payload_equal(const mstore_schema_t *schema, const void *lhs, const void *rhs) {
    if (schema->payload_equals != NULL) {
        return schema->payload_equals(lhs, rhs);
    }
    return memcmp(lhs, rhs, schema->payload_size) == 0;
}

static void mstore_state_destroy(mstore_state_t *st) {
    if (st == NULL) {
        return;
    }
    if (st->lock != NULL) {
        mstore_platform_lock_destroy(st->lock);
    }
    mstore_platform_free(st->index);
    mstore_platform_free(st->free_slots);
    mstore_platform_free(st->slots);
    mstore_platform_free(st);
}

mstore_err_t mstore_table_init(mstore_table_t *table, const mstore_table_schema_t *schema) {
    if (table == NULL || schema == NULL) {
        return MSTORE_INVALID_ARG;
    }
    if (table->_state != NULL) {
        return MSTORE_INVALID_STATE;
    }
    if (schema->capacity == 0 || schema->key_size == 0) {
        return MSTORE_INVALID_SIZE;
    }

    mstore_state_t *st = mstore_platform_alloc(sizeof(*st));
    if (st == NULL) {
        return MSTORE_NO_MEM;
    }
    memset(st, 0, sizeof(*st));

    st->schema.capacity = schema->capacity;
    st->schema.key_size = schema->key_size;
    st->schema.payload_size = schema->payload_size;
    st->schema.payload_equals = schema->payload_equals;
    st->schema.slot_size = mstore_align_up(
        sizeof(mstore_meta_t) + schema->key_size + schema->payload_size, _Alignof(mstore_meta_t));
    st->index_capacity = mstore_next_pow2(schema->capacity * 2);

    st->slots = mstore_platform_alloc(st->schema.slot_size * schema->capacity);
    st->free_slots = mstore_platform_alloc(sizeof(mstore_slot_t) * schema->capacity);
    st->index = mstore_platform_alloc(sizeof(mstore_slot_t) * st->index_capacity);
    st->lock = mstore_platform_lock_create();

    if (st->slots == NULL || st->free_slots == NULL || st->index == NULL || st->lock == NULL) {
        mstore_state_destroy(st);
        return MSTORE_NO_MEM;
    }

    memset(st->slots, 0, st->schema.slot_size * schema->capacity);
    mstore_runtime_rebuild(st);
    table->_state = st;
    return MSTORE_OK;
}

mstore_err_t mstore_table_deinit(mstore_table_t *table) {
    if (table == NULL) {
        return MSTORE_INVALID_ARG;
    }
    if (table->_state == NULL) {
        return MSTORE_INVALID_STATE;
    }
    mstore_state_destroy((mstore_state_t *)table->_state);
    table->_state = NULL;
    return MSTORE_OK;
}

mstore_err_t mstore_table_count(const mstore_table_t *table, size_t *out_count) {
    if (table == NULL || out_count == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }
    mstore_platform_lock_acquire(st->lock);
    *out_count = st->live_count;
    mstore_platform_lock_release(st->lock);
    return MSTORE_OK;
}

mstore_err_t mstore_table_clear(mstore_table_t *table) {
    if (table == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }
    mstore_platform_lock_acquire(st->lock);
    for (size_t i = 0; i < st->schema.capacity; i++) {
        mstore_slot_meta_mut(st, (mstore_slot_t)i)->used = false;
    }
    mstore_runtime_rebuild(st);
    mstore_platform_lock_release(st->lock);
    return MSTORE_OK;
}

mstore_err_t mstore_table_slot_find(const mstore_table_t *table, const void *key,
                                    mstore_slot_t *out_slot) {
    if (table == NULL || key == NULL || out_slot == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }
    mstore_platform_lock_acquire(st->lock);
    mstore_err_t err = mstore_index_find(st, key, out_slot);
    mstore_platform_lock_release(st->lock);
    return err;
}

mstore_err_t mstore_table_slot_meta(const mstore_table_t *table, mstore_slot_t slot,
                                    mstore_meta_t *out_meta) {
    if (table == NULL || out_meta == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }
    if (slot >= st->schema.capacity) {
        return MSTORE_INVALID_ARG;
    }

    mstore_platform_lock_acquire(st->lock);
    const mstore_meta_t *meta = mstore_slot_meta(st, slot);
    mstore_err_t err = meta->used ? MSTORE_OK : MSTORE_STALE;
    if (err == MSTORE_OK) {
        *out_meta = *meta;
    }
    mstore_platform_lock_release(st->lock);
    return err;
}

mstore_err_t mstore_table_slot_read(const mstore_table_t *table, mstore_slot_t slot,
                                    mstore_meta_t *out_meta, void *out_key, void *out_payload) {
    if (table == NULL || out_meta == NULL || out_key == NULL || out_payload == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }
    if (slot >= st->schema.capacity) {
        return MSTORE_INVALID_ARG;
    }

    mstore_platform_lock_acquire(st->lock);
    const mstore_meta_t *meta = mstore_slot_meta(st, slot);
    mstore_err_t err = MSTORE_STALE;
    if (meta->used) {
        *out_meta = *meta;
        memcpy(out_key, mstore_slot_key(st, slot), st->schema.key_size);
        memcpy(out_payload, mstore_slot_payload(st, slot), st->schema.payload_size);
        err = MSTORE_OK;
    }
    mstore_platform_lock_release(st->lock);
    return err;
}

mstore_err_t mstore_table_slot_allocate(mstore_table_t *table, const void *key, const void *payload,
                                        mstore_slot_t *out_slot, uint32_t *out_generation) {
    if (table == NULL || key == NULL || payload == NULL || out_slot == NULL ||
        out_generation == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    mstore_platform_lock_acquire(st->lock);

    mstore_slot_t existing;
    if (mstore_index_find(st, key, &existing) == MSTORE_OK) {
        mstore_platform_lock_release(st->lock);
        return MSTORE_ALREADY_EXISTS;
    }

    mstore_slot_t slot;
    mstore_err_t err = mstore_freelist_pop(st, &slot);
    if (err != MSTORE_OK) {
        mstore_platform_lock_release(st->lock);
        return err;
    }

    mstore_meta_t *meta = mstore_slot_meta_mut(st, slot);
    meta->used = true;
    meta->generation++;
    meta->version = 1;
    memcpy(mstore_slot_key_mut(st, slot), key, st->schema.key_size);
    memcpy(mstore_slot_payload_mut(st, slot), payload, st->schema.payload_size);

    mstore_index_insert(st, slot);
    st->live_count++;

    *out_slot = slot;
    *out_generation = meta->generation;

    mstore_platform_lock_release(st->lock);
    return MSTORE_OK;
}

mstore_err_t mstore_table_slot_update(mstore_table_t *table, mstore_slot_t slot,
                                      uint32_t expected_generation, const void *payload,
                                      bool *out_changed) {
    if (table == NULL || payload == NULL || out_changed == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }
    if (slot >= st->schema.capacity) {
        return MSTORE_INVALID_ARG;
    }

    mstore_platform_lock_acquire(st->lock);
    mstore_meta_t *meta = mstore_slot_meta_mut(st, slot);
    if (!meta->used || meta->generation != expected_generation) {
        mstore_platform_lock_release(st->lock);
        return MSTORE_STALE;
    }

    bool changed = !mstore_payload_equal(&st->schema, mstore_slot_payload(st, slot), payload);
    if (changed) {
        memcpy(mstore_slot_payload_mut(st, slot), payload, st->schema.payload_size);
        meta->version++;
    }
    *out_changed = changed;

    mstore_platform_lock_release(st->lock);
    return MSTORE_OK;
}

mstore_err_t mstore_table_slot_free(mstore_table_t *table, mstore_slot_t slot,
                                    uint32_t expected_generation) {
    if (table == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }
    if (slot >= st->schema.capacity) {
        return MSTORE_INVALID_ARG;
    }

    mstore_platform_lock_acquire(st->lock);
    mstore_meta_t *meta = mstore_slot_meta_mut(st, slot);
    if (!meta->used || meta->generation != expected_generation) {
        mstore_platform_lock_release(st->lock);
        return MSTORE_STALE;
    }

    mstore_index_remove(st, slot);
    meta->used = false;
    mstore_freelist_push(st, slot);
    st->live_count--;

    mstore_platform_lock_release(st->lock);
    return MSTORE_OK;
}

mstore_err_t mstore_table_iter(const mstore_table_t *table, mstore_iter_cb_t cb, void *ctx) {
    if (table == NULL || cb == NULL) {
        return MSTORE_INVALID_ARG;
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    mstore_platform_lock_acquire(st->lock);
    for (size_t i = 0; i < st->schema.capacity; i++) {
        mstore_slot_t slot = (mstore_slot_t)i;
        const mstore_meta_t *meta = mstore_slot_meta(st, slot);
        if (!meta->used) {
            continue;
        }
        if (!cb(slot, meta, mstore_slot_payload(st, slot), ctx)) {
            break;
        }
    }
    mstore_platform_lock_release(st->lock);
    return MSTORE_OK;
}
