#include <string.h>

#include "mstore/mstore_table.h"
#include "mstore_internal.h"

/* 0 — значение не представимо (переполнение разрядной сетки). */
static size_t mstore_next_pow2(size_t value) {
    size_t result = 8;
    while (result < value) {
        if (result > SIZE_MAX / 2) {
            return 0;
        }
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
    mstore_storage_close(st->storage);
    mstore_platform_free(st->scratch_payload);
    mstore_platform_free(st->scratch_lookup);
    mstore_platform_free(st->scratch_key);
    mstore_platform_free(st->index_of_slot);
    mstore_platform_free(st->index);
    mstore_platform_free(st->free_slots);
    if (st->lock != NULL) {
        mstore_platform_lock_destroy(st->lock);
    }
    mstore_platform_free(st);
}

sys_error_t mstore_table_init(mstore_table_t *table, const mstore_table_schema_t *schema) {
    if (table == NULL || schema == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    if (table->_state != NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }
    if (schema->capacity == 0 || schema->key_size == 0) {
        return mstore_fail(SYS_CODE_INVALID_SIZE);
    }
    if (schema->capacity > (size_t)UINT32_MAX || schema->capacity > SIZE_MAX / 2) {
        return mstore_fail(SYS_CODE_INVALID_SIZE);
    }
    if (schema->backing != MSTORE_BACKING_RAM && schema->backing != MSTORE_BACKING_FLASH &&
        schema->backing != (MSTORE_BACKING_RAM | MSTORE_BACKING_FLASH)) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }

    mstore_state_t *st = mstore_platform_alloc(sizeof(*st));
    if (st == NULL) {
        return mstore_fail(SYS_CODE_NO_MEM);
    }
    memset(st, 0, sizeof(*st));

    st->schema.capacity = schema->capacity;
    st->schema.key_size = schema->key_size;
    st->schema.payload_size = schema->payload_size;
    st->schema.payload_equals = schema->payload_equals;
    st->backing = schema->backing;
    st->index_capacity = mstore_next_pow2(schema->capacity * 2);
    if (st->index_capacity == 0 || st->index_capacity > SIZE_MAX / sizeof(mstore_index_entry_t) ||
        schema->capacity > SIZE_MAX / sizeof(uint32_t)) {
        mstore_platform_free(st);
        return mstore_fail(SYS_CODE_INVALID_SIZE);
    }

    mstore_storage_config_t storage_config;
    storage_config.backing = schema->backing;
    storage_config.capacity = schema->capacity;
    storage_config.key_size = schema->key_size;
    storage_config.payload_size = schema->payload_size;
    storage_config.persist_key = schema->persist_key;

    sys_error_t err = mstore_storage_open(&storage_config, &st->storage);
    if (sys_failed(err)) {
        mstore_state_destroy(st);
        return err;
    }

    st->free_slots = mstore_platform_alloc(sizeof(mstore_slot_t) * schema->capacity);
    st->index = mstore_platform_alloc(sizeof(mstore_index_entry_t) * st->index_capacity);
    st->index_of_slot = mstore_platform_alloc(sizeof(uint32_t) * schema->capacity);
    st->scratch_key = mstore_platform_alloc(schema->key_size);
    st->scratch_lookup = mstore_platform_alloc(schema->key_size);
    st->scratch_payload = mstore_platform_alloc(schema->payload_size == 0 ? 1 : schema->payload_size);
    st->lock = mstore_platform_lock_create();

    if (st->free_slots == NULL || st->index == NULL || st->index_of_slot == NULL ||
        st->scratch_key == NULL || st->scratch_lookup == NULL || st->scratch_payload == NULL ||
        st->lock == NULL) {
        mstore_state_destroy(st);
        return mstore_fail(SYS_CODE_NO_MEM);
    }

    err = mstore_runtime_rebuild(st);
    if (sys_failed(err)) {
        mstore_state_destroy(st);
        return err;
    }

    table->_state = st;
    return SYS_OK;
}

sys_error_t mstore_table_deinit(mstore_table_t *table) {
    if (table == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    if (table->_state == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }
    mstore_state_destroy((mstore_state_t *)table->_state);
    table->_state = NULL;
    return SYS_OK;
}

sys_error_t mstore_table_count(const mstore_table_t *table, size_t *out_count) {
    if (table == NULL || out_count == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }
    mstore_platform_lock_acquire(st->lock);
    *out_count = st->live_count;
    mstore_platform_lock_release(st->lock);
    return SYS_OK;
}

sys_error_t mstore_table_clear(mstore_table_t *table) {
    if (table == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }
    mstore_platform_lock_acquire(st->lock);
    sys_error_t err = mstore_storage_clear_all(st->storage);
    if (sys_ok(err)) {
        /* После успешного clear_all runtime reset детерминирован и без I/O. */
        mstore_runtime_reset(st);
    }
    mstore_platform_lock_release(st->lock);
    return err;
}

sys_error_t mstore_table_slot_find(const mstore_table_t *table, const void *key,
                                    mstore_slot_t *out_slot) {
    if (table == NULL || key == NULL || out_slot == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }
    mstore_platform_lock_acquire(st->lock);
    sys_error_t err = mstore_index_find(st, key, st->scratch_key, out_slot);
    mstore_platform_lock_release(st->lock);
    return err;
}

sys_error_t mstore_table_slot_meta(const mstore_table_t *table, mstore_slot_t slot,
                                    mstore_meta_t *out_meta) {
    if (table == NULL || out_meta == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }
    if (slot >= st->schema.capacity) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }

    mstore_platform_lock_acquire(st->lock);
    mstore_meta_t meta;
    sys_error_t err = mstore_storage_read_meta(st->storage, slot, &meta);
    if (sys_ok(err)) {
        if (meta.used) {
            *out_meta = meta;
        } else {
            err = mstore_fail(SYS_CODE_STALE);
        }
    }
    mstore_platform_lock_release(st->lock);
    return err;
}

sys_error_t mstore_table_slot_read(const mstore_table_t *table, mstore_slot_t slot,
                                    mstore_meta_t *out_meta, void *out_key, void *out_payload) {
    if (table == NULL || out_meta == NULL || out_key == NULL || out_payload == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }
    if (slot >= st->schema.capacity) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }

    mstore_platform_lock_acquire(st->lock);
    mstore_meta_t meta;
    sys_error_t err = mstore_storage_read_meta(st->storage, slot, &meta);
    if (sys_ok(err)) {
        if (meta.used) {
            err = mstore_storage_read_slot(st->storage, slot, out_meta, out_key, out_payload);
        } else {
            err = mstore_fail(SYS_CODE_STALE);
        }
    }
    mstore_platform_lock_release(st->lock);
    return err;
}

sys_error_t mstore_table_slot_allocate(mstore_table_t *table, const void *key, const void *payload,
                                        mstore_slot_t *out_slot, uint32_t *out_generation) {
    if (table == NULL || key == NULL || payload == NULL || out_slot == NULL ||
        out_generation == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }

    mstore_platform_lock_acquire(st->lock);

    mstore_slot_t existing;
    sys_error_t err = mstore_index_find(st, key, st->scratch_key, &existing);
    if (sys_ok(err)) {
        mstore_platform_lock_release(st->lock);
        return mstore_fail(SYS_CODE_ALREADY_EXISTS);
    }
    if (!sys_is(err, SYS_CODE_NOT_FOUND)) {
        mstore_platform_lock_release(st->lock);
        return err;
    }

    if (st->free_count == 0) {
        mstore_platform_lock_release(st->lock);
        return mstore_fail(SYS_CODE_NO_SPACE);
    }

    /* Берём первый свободный slot с неисчерпанной generation; исчерпанные
     * остаются в free-list, но больше не переиспользуются (без ABA). */
    size_t pick = SIZE_MAX;
    mstore_meta_t meta = {0};
    for (size_t i = st->free_count; i > 0; i--) {
        mstore_slot_t candidate = st->free_slots[i - 1];
        err = mstore_storage_read_meta(st->storage, candidate, &meta);
        if (sys_failed(err)) {
            mstore_platform_lock_release(st->lock);
            return err;
        }
        if (meta.generation != UINT32_MAX) {
            pick = i - 1;
            break;
        }
    }
    if (pick == SIZE_MAX) {
        mstore_platform_lock_release(st->lock);
        return mstore_fail(SYS_CODE_OVERFLOW);
    }

    mstore_slot_t slot = st->free_slots[pick];
    meta.used = true;
    meta.generation++;
    meta.version = 1;

    err = mstore_storage_write_slot(st->storage, slot, &meta, key, payload);
    if (sys_failed(err)) {
        mstore_platform_lock_release(st->lock);
        return err;
    }

    st->free_slots[pick] = st->free_slots[st->free_count - 1];
    st->free_count--;
    mstore_index_insert(st, slot, key);
    st->live_count++;

    *out_slot = slot;
    *out_generation = meta.generation;
    mstore_platform_lock_release(st->lock);
    return SYS_OK;
}

sys_error_t mstore_table_slot_update(mstore_table_t *table, mstore_slot_t slot,
                                      uint32_t expected_generation, const void *payload,
                                      bool *out_changed) {
    if (table == NULL || payload == NULL || out_changed == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }
    if (slot >= st->schema.capacity) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }

    mstore_platform_lock_acquire(st->lock);
    mstore_meta_t meta;
    sys_error_t err = mstore_storage_read_meta(st->storage, slot, &meta);
    if (sys_failed(err)) {
        mstore_platform_lock_release(st->lock);
        return err;
    }
    if (!meta.used || meta.generation != expected_generation) {
        mstore_platform_lock_release(st->lock);
        return mstore_fail(SYS_CODE_STALE);
    }

    err = mstore_storage_read_slot(st->storage, slot, &meta, st->scratch_key, st->scratch_payload);
    if (sys_failed(err)) {
        mstore_platform_lock_release(st->lock);
        return err;
    }

    bool changed = !mstore_payload_equal(&st->schema, st->scratch_payload, payload);
    if (changed) {
        if (meta.version == UINT32_MAX) {
            mstore_platform_lock_release(st->lock);
            return mstore_fail(SYS_CODE_OVERFLOW);
        }
        meta.version++;
        err = mstore_storage_write_slot(st->storage, slot, &meta, st->scratch_key, payload);
        if (sys_failed(err)) {
            mstore_platform_lock_release(st->lock);
            return err;
        }
    }
    *out_changed = changed;
    mstore_platform_lock_release(st->lock);
    return SYS_OK;
}

sys_error_t mstore_table_slot_free(mstore_table_t *table, mstore_slot_t slot,
                                    uint32_t expected_generation) {
    if (table == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }
    if (slot >= st->schema.capacity) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }

    mstore_platform_lock_acquire(st->lock);
    mstore_meta_t meta;
    sys_error_t err = mstore_storage_read_meta(st->storage, slot, &meta);
    if (sys_failed(err)) {
        mstore_platform_lock_release(st->lock);
        return err;
    }
    if (!meta.used || meta.generation != expected_generation) {
        mstore_platform_lock_release(st->lock);
        return mstore_fail(SYS_CODE_STALE);
    }

    meta.used = false;
    err = mstore_storage_write_meta(st->storage, slot, &meta);
    if (sys_failed(err)) {
        mstore_platform_lock_release(st->lock);
        return err;
    }

    /* post-commit derived update: без I/O */
    mstore_index_remove(st, slot);
    mstore_freelist_push(st, slot);
    st->live_count--;

    mstore_platform_lock_release(st->lock);
    return SYS_OK;
}

sys_error_t mstore_table_iter(const mstore_table_t *table, mstore_iter_cb_t cb, void *ctx) {
    if (table == NULL || cb == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    mstore_state_t *st = table->_state;
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }

    mstore_platform_lock_acquire(st->lock);
    sys_error_t err = SYS_OK;
    for (size_t i = 0; i < st->schema.capacity; i++) {
        mstore_slot_t slot = (mstore_slot_t)i;
        mstore_meta_t meta;
        err = mstore_storage_read_meta(st->storage, slot, &meta);
        if (sys_failed(err)) {
            break;
        }
        if (!meta.used) {
            continue;
        }
        err = mstore_storage_read_slot(st->storage, slot, &meta, st->scratch_key,
                                       st->scratch_payload);
        if (sys_failed(err)) {
            break;
        }
        if (!cb(slot, &meta, st->scratch_key, st->scratch_payload, ctx)) {
            break;
        }
    }
    mstore_platform_lock_release(st->lock);
    return err;
}
