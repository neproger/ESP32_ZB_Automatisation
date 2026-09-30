#include <string.h>

#include "mstore/mstore_table.h"
#include "mstore_internal.h"

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
    mstore_storage_close(st->storage);
    mstore_platform_free(st->scratch_payload);
    mstore_platform_free(st->scratch_key);
    mstore_platform_free(st->index_of_slot);
    mstore_platform_free(st->index);
    mstore_platform_free(st->free_slots);
    if (st->lock != NULL) {
        mstore_platform_lock_destroy(st->lock);
    }
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
    if (schema->capacity > (size_t)UINT32_MAX || schema->capacity > SIZE_MAX / 2) {
        return MSTORE_INVALID_SIZE;
    }
    if (schema->backing != MSTORE_BACKING_RAM && schema->backing != MSTORE_BACKING_FLASH &&
        schema->backing != (MSTORE_BACKING_RAM | MSTORE_BACKING_FLASH)) {
        return MSTORE_INVALID_ARG;
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
    st->backing = schema->backing;
    st->index_capacity = mstore_next_pow2(schema->capacity * 2);

    mstore_storage_config_t storage_config;
    storage_config.backing = schema->backing;
    storage_config.capacity = schema->capacity;
    storage_config.key_size = schema->key_size;
    storage_config.payload_size = schema->payload_size;
    storage_config.persist_key = schema->persist_key;

    mstore_err_t err = mstore_storage_open(&storage_config, &st->storage);
    if (err != MSTORE_OK) {
        mstore_state_destroy(st);
        return err;
    }

    st->free_slots = mstore_platform_alloc(sizeof(mstore_slot_t) * schema->capacity);
    st->index = mstore_platform_alloc(sizeof(mstore_index_entry_t) * st->index_capacity);
    st->index_of_slot = mstore_platform_alloc(sizeof(uint32_t) * schema->capacity);
    st->scratch_key = mstore_platform_alloc(schema->key_size);
    st->scratch_payload = mstore_platform_alloc(schema->payload_size == 0 ? 1 : schema->payload_size);
    st->lock = mstore_platform_lock_create();

    if (st->free_slots == NULL || st->index == NULL || st->index_of_slot == NULL ||
        st->scratch_key == NULL || st->scratch_payload == NULL || st->lock == NULL) {
        mstore_state_destroy(st);
        return MSTORE_NO_MEM;
    }

    err = mstore_runtime_rebuild(st);
    if (err != MSTORE_OK) {
        mstore_state_destroy(st);
        return err;
    }

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
    mstore_err_t err = mstore_storage_clear_all(st->storage);
    if (err == MSTORE_OK) {
        /* После успешного clear_all runtime reset детерминирован и без I/O. */
        mstore_runtime_reset(st);
    }
    mstore_platform_lock_release(st->lock);
    return err;
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
    mstore_meta_t meta;
    mstore_err_t err = mstore_storage_read_meta(st->storage, slot, &meta);
    if (err == MSTORE_OK) {
        if (meta.used) {
            *out_meta = meta;
        } else {
            err = MSTORE_STALE;
        }
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
    mstore_meta_t meta;
    mstore_err_t err = mstore_storage_read_meta(st->storage, slot, &meta);
    if (err == MSTORE_OK) {
        if (meta.used) {
            err = mstore_storage_read_slot(st->storage, slot, out_meta, out_key, out_payload);
        } else {
            err = MSTORE_STALE;
        }
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
    mstore_err_t err = mstore_index_find(st, key, &existing);
    if (err == MSTORE_OK) {
        mstore_platform_lock_release(st->lock);
        return MSTORE_ALREADY_EXISTS;
    }
    if (err != MSTORE_NOT_FOUND) {
        mstore_platform_lock_release(st->lock);
        return err;
    }

    if (st->free_count == 0) {
        mstore_platform_lock_release(st->lock);
        return MSTORE_NO_SPACE;
    }
    mstore_slot_t slot = st->free_slots[st->free_count - 1];

    mstore_meta_t meta;
    err = mstore_storage_read_meta(st->storage, slot, &meta);
    if (err != MSTORE_OK) {
        mstore_platform_lock_release(st->lock);
        return err;
    }
    meta.used = true;
    meta.generation++;
    meta.version = 1;

    err = mstore_storage_write_slot(st->storage, slot, &meta, key, payload);
    if (err != MSTORE_OK) {
        mstore_platform_lock_release(st->lock);
        return err;
    }

    st->free_count--;
    mstore_index_insert(st, slot, key);
    st->live_count++;

    *out_slot = slot;
    *out_generation = meta.generation;
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
    mstore_meta_t meta;
    mstore_err_t err = mstore_storage_read_meta(st->storage, slot, &meta);
    if (err != MSTORE_OK) {
        mstore_platform_lock_release(st->lock);
        return err;
    }
    if (!meta.used || meta.generation != expected_generation) {
        mstore_platform_lock_release(st->lock);
        return MSTORE_STALE;
    }

    err = mstore_storage_read_slot(st->storage, slot, &meta, st->scratch_key, st->scratch_payload);
    if (err != MSTORE_OK) {
        mstore_platform_lock_release(st->lock);
        return err;
    }

    bool changed = !mstore_payload_equal(&st->schema, st->scratch_payload, payload);
    if (changed) {
        meta.version++;
        err = mstore_storage_write_slot(st->storage, slot, &meta, st->scratch_key, payload);
        if (err != MSTORE_OK) {
            mstore_platform_lock_release(st->lock);
            return err;
        }
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
    mstore_meta_t meta;
    mstore_err_t err = mstore_storage_read_meta(st->storage, slot, &meta);
    if (err != MSTORE_OK) {
        mstore_platform_lock_release(st->lock);
        return err;
    }
    if (!meta.used || meta.generation != expected_generation) {
        mstore_platform_lock_release(st->lock);
        return MSTORE_STALE;
    }

    meta.used = false;
    err = mstore_storage_write_meta(st->storage, slot, &meta);
    if (err != MSTORE_OK) {
        mstore_platform_lock_release(st->lock);
        return err;
    }

    /* post-commit derived update: без I/O */
    mstore_index_remove(st, slot);
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
    mstore_err_t err = MSTORE_OK;
    for (size_t i = 0; i < st->schema.capacity; i++) {
        mstore_slot_t slot = (mstore_slot_t)i;
        mstore_meta_t meta;
        err = mstore_storage_read_meta(st->storage, slot, &meta);
        if (err != MSTORE_OK) {
            break;
        }
        if (!meta.used) {
            continue;
        }
        err = mstore_storage_read_slot(st->storage, slot, &meta, st->scratch_key,
                                       st->scratch_payload);
        if (err != MSTORE_OK) {
            break;
        }
        if (!cb(slot, &meta, st->scratch_payload, ctx)) {
            break;
        }
    }
    mstore_platform_lock_release(st->lock);
    return err;
}
