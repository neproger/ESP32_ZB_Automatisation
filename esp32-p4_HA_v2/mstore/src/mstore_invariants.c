#include "mstore_internal.h"

/* Возвращает MSTORE_INVARIANT_FAILED при нарушении структуры и ошибку storage,
 * если read не удался (это разные исходы). */
mstore_err_t mstore_check_invariants(mstore_state_t *st) {
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    size_t used_count = 0;
    for (size_t i = 0; i < st->schema.capacity; i++) {
        mstore_slot_t slot = (mstore_slot_t)i;
        mstore_meta_t meta;
        mstore_err_t err = mstore_storage_read_meta(st->storage, slot, &meta);
        if (err != MSTORE_OK) {
            return err;
        }
        if (!meta.used) {
            continue;
        }
        used_count++;

        err = mstore_storage_read_key(st->storage, slot, st->scratch_key);
        if (err != MSTORE_OK) {
            return err;
        }
        mstore_slot_t found;
        err = mstore_index_find(st, st->scratch_key, &found);
        if (err == MSTORE_NOT_FOUND || (err == MSTORE_OK && found != slot)) {
            return MSTORE_INVARIANT_FAILED;
        }
        if (err != MSTORE_OK) {
            return err;
        }
    }
    if (used_count != st->live_count) {
        return MSTORE_INVARIANT_FAILED;
    }

    for (size_t i = 0; i < st->index_capacity; i++) {
        mstore_slot_t slot = st->index[i];
        if (slot == MSTORE_SLOT_NONE) {
            continue;
        }
        if (slot >= st->schema.capacity) {
            return MSTORE_INVARIANT_FAILED;
        }
        mstore_meta_t meta;
        mstore_err_t err = mstore_storage_read_meta(st->storage, slot, &meta);
        if (err != MSTORE_OK) {
            return err;
        }
        if (!meta.used) {
            return MSTORE_INVARIANT_FAILED;
        }
    }

    if (st->free_count > st->schema.capacity) {
        return MSTORE_INVARIANT_FAILED;
    }
    for (size_t i = 0; i < st->free_count; i++) {
        mstore_slot_t slot = st->free_slots[i];
        if (slot >= st->schema.capacity) {
            return MSTORE_INVARIANT_FAILED;
        }
        mstore_meta_t meta;
        mstore_err_t err = mstore_storage_read_meta(st->storage, slot, &meta);
        if (err != MSTORE_OK) {
            return err;
        }
        if (meta.used) {
            return MSTORE_INVARIANT_FAILED;
        }
        for (size_t j = i + 1; j < st->free_count; j++) {
            if (st->free_slots[j] == slot) {
                return MSTORE_INVARIANT_FAILED;
            }
        }
    }

    if (st->free_count + st->live_count != st->schema.capacity) {
        return MSTORE_INVARIANT_FAILED;
    }
    return MSTORE_OK;
}
