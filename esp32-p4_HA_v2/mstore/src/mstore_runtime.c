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

/* true, если home лежит в циклическом интервале (hole, cursor]. */
static bool mstore_home_reachable(size_t hole, size_t cursor, size_t home) {
    if (cursor > hole) {
        return home > hole && home <= cursor;
    }
    return home > hole || home <= cursor;
}

sys_error_t mstore_index_find(mstore_state_t *st, const void *key, void *probe_key,
                               mstore_slot_t *out_slot) {
    size_t mask = st->index_capacity - 1;
    size_t bucket = mstore_hash_key(&st->schema, key) & mask;
    size_t probes = 0;
    while (st->index[bucket].slot != MSTORE_SLOT_NONE) {
        if (probes++ >= st->index_capacity) {
            return mstore_fail(SYS_CODE_INVARIANT_FAILED);
        }
        mstore_slot_t slot = st->index[bucket].slot;
        sys_error_t err = mstore_storage_read_key(st->storage, slot, probe_key);
        if (sys_failed(err)) {
            return err;
        }
        if (mstore_key_equal(&st->schema, probe_key, key)) {
            *out_slot = slot;
            return SYS_OK;
        }
        bucket = (bucket + 1) & mask;
    }
    return mstore_fail(SYS_CODE_NOT_FOUND);
}

void mstore_index_insert(mstore_state_t *st, mstore_slot_t slot, const void *key) {
    size_t mask = st->index_capacity - 1;
    size_t home = mstore_hash_key(&st->schema, key) & mask;
    size_t bucket = home;
    while (st->index[bucket].slot != MSTORE_SLOT_NONE) {
        bucket = (bucket + 1) & mask;
    }
    st->index[bucket].slot = slot;
    st->index[bucket].home = (uint32_t)home;
    st->index_of_slot[slot] = (uint32_t)bucket;
}

void mstore_index_remove(mstore_state_t *st, mstore_slot_t slot) {
    uint32_t bucket = st->index_of_slot[slot];
    if (bucket == MSTORE_INDEX_NONE || st->index[bucket].slot != slot) {
        return;
    }

    size_t mask = st->index_capacity - 1;
    size_t hole = bucket;
    size_t cursor = hole;
    for (;;) {
        cursor = (cursor + 1) & mask;
        if (st->index[cursor].slot == MSTORE_SLOT_NONE) {
            break;
        }
        size_t home = st->index[cursor].home;
        if (!mstore_home_reachable(hole, cursor, home)) {
            st->index[hole] = st->index[cursor];
            st->index_of_slot[st->index[hole].slot] = (uint32_t)hole;
            hole = cursor;
        }
    }
    st->index[hole].slot = MSTORE_SLOT_NONE;
    st->index_of_slot[slot] = MSTORE_INDEX_NONE;
}

sys_error_t mstore_freelist_pop(mstore_state_t *st, mstore_slot_t *out_slot) {
    if (st->free_count == 0) {
        return mstore_fail(SYS_CODE_NO_SPACE);
    }
    st->free_count--;
    *out_slot = st->free_slots[st->free_count];
    return SYS_OK;
}

void mstore_freelist_push(mstore_state_t *st, mstore_slot_t slot) {
    st->free_slots[st->free_count] = slot;
    st->free_count++;
}

void mstore_runtime_reset(mstore_state_t *st) {
    for (size_t i = 0; i < st->index_capacity; i++) {
        st->index[i].slot = MSTORE_SLOT_NONE;
        st->index[i].home = 0;
    }
    for (size_t slot = 0; slot < st->schema.capacity; slot++) {
        st->index_of_slot[slot] = MSTORE_INDEX_NONE;
        st->free_slots[slot] = (mstore_slot_t)slot;
    }
    st->free_count = st->schema.capacity;
    st->live_count = 0;
}

sys_error_t mstore_runtime_rebuild(mstore_state_t *st) {
    for (size_t i = 0; i < st->index_capacity; i++) {
        st->index[i].slot = MSTORE_SLOT_NONE;
        st->index[i].home = 0;
    }
    for (size_t slot = 0; slot < st->schema.capacity; slot++) {
        st->index_of_slot[slot] = MSTORE_INDEX_NONE;
    }
    st->free_count = 0;
    st->live_count = 0;

    for (size_t i = 0; i < st->schema.capacity; i++) {
        mstore_slot_t slot = (mstore_slot_t)i;
        mstore_meta_t meta;
        sys_error_t err = mstore_storage_read_meta(st->storage, slot, &meta);
        if (sys_failed(err)) {
            return err;
        }
        if (meta.used) {
            err = mstore_storage_read_key(st->storage, slot, st->scratch_key);
            if (sys_failed(err)) {
                return err;
            }
            mstore_index_insert(st, slot, st->scratch_key);
            st->live_count++;
        } else {
            mstore_freelist_push(st, slot);
        }
    }
    return SYS_OK;
}
