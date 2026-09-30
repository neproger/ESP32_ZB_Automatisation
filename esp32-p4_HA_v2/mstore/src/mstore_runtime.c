#include <string.h>

#include "mstore_internal.h"

static uint32_t mstore_hash_key(const mstore_schema_t *schema, const void *key) {
    const uint8_t *bytes = key;
    uint32_t hash = 2166136261u; /* FNV-1a */
    for (size_t i = 0; i < schema->key_size; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool mstore_key_equal(const mstore_schema_t *schema, const void *lhs, const void *rhs) {
    return memcmp(lhs, rhs, schema->key_size) == 0;
}

mstore_err_t mstore_index_find(mstore_state_t *st, const void *key, mstore_slot_t *out_slot) {
    size_t mask = st->index_capacity - 1;
    size_t bucket = mstore_hash_key(&st->schema, key) & mask;
    while (st->index[bucket] != MSTORE_SLOT_NONE) {
        mstore_slot_t slot = st->index[bucket];
        mstore_err_t err = mstore_storage_read_key(st->storage, slot, st->scratch_key);
        if (err != MSTORE_OK) {
            return err;
        }
        if (mstore_key_equal(&st->schema, st->scratch_key, key)) {
            *out_slot = slot;
            return MSTORE_OK;
        }
        bucket = (bucket + 1) & mask;
    }
    return MSTORE_NOT_FOUND;
}

mstore_err_t mstore_index_insert(mstore_state_t *st, mstore_slot_t slot, const void *key) {
    size_t mask = st->index_capacity - 1;
    size_t bucket = mstore_hash_key(&st->schema, key) & mask;
    while (st->index[bucket] != MSTORE_SLOT_NONE) {
        bucket = (bucket + 1) & mask;
    }
    st->index[bucket] = slot;
    return MSTORE_OK;
}

/* true, если home лежит в циклическом интервале (hole, cursor]. */
static bool mstore_home_reachable(size_t hole, size_t cursor, size_t home) {
    if (cursor > hole) {
        return home > hole && home <= cursor;
    }
    return home > hole || home <= cursor;
}

mstore_err_t mstore_index_remove(mstore_state_t *st, mstore_slot_t slot, const void *key) {
    size_t mask = st->index_capacity - 1;
    size_t hole = mstore_hash_key(&st->schema, key) & mask;
    while (st->index[hole] != slot) {
        hole = (hole + 1) & mask;
    }

    size_t cursor = hole;
    for (;;) {
        cursor = (cursor + 1) & mask;
        mstore_slot_t entry = st->index[cursor];
        if (entry == MSTORE_SLOT_NONE) {
            break;
        }
        mstore_err_t err = mstore_storage_read_key(st->storage, entry, st->scratch_key);
        if (err != MSTORE_OK) {
            return err;
        }
        size_t home = mstore_hash_key(&st->schema, st->scratch_key) & mask;
        if (!mstore_home_reachable(hole, cursor, home)) {
            st->index[hole] = entry;
            hole = cursor;
        }
    }
    st->index[hole] = MSTORE_SLOT_NONE;
    return MSTORE_OK;
}

mstore_err_t mstore_freelist_pop(mstore_state_t *st, mstore_slot_t *out_slot) {
    if (st->free_count == 0) {
        return MSTORE_NO_SPACE;
    }
    st->free_count--;
    *out_slot = st->free_slots[st->free_count];
    return MSTORE_OK;
}

void mstore_freelist_push(mstore_state_t *st, mstore_slot_t slot) {
    st->free_slots[st->free_count] = slot;
    st->free_count++;
}

mstore_err_t mstore_runtime_rebuild(mstore_state_t *st) {
    memset(st->index, 0xFF, st->index_capacity * sizeof(mstore_slot_t));
    st->free_count = 0;
    st->live_count = 0;

    for (size_t i = 0; i < st->schema.capacity; i++) {
        mstore_slot_t slot = (mstore_slot_t)i;
        mstore_meta_t meta;
        mstore_err_t err = mstore_storage_read_meta(st->storage, slot, &meta);
        if (err != MSTORE_OK) {
            return err;
        }
        if (meta.used) {
            err = mstore_storage_read_key(st->storage, slot, st->scratch_key);
            if (err != MSTORE_OK) {
                return err;
            }
            mstore_index_insert(st, slot, st->scratch_key);
            st->live_count++;
        } else {
            mstore_freelist_push(st, slot);
        }
    }
    return MSTORE_OK;
}
