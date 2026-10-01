#include "mstore_internal.h"

/* Возвращает INVARIANT_FAILED при нарушении структуры и ошибку storage,
 * если read не удался (это разные исходы). */
sys_error_t mstore_check_invariants(mstore_state_t *st) {
    if (st == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }

    size_t used_count = 0;
    for (size_t i = 0; i < st->schema.capacity; i++) {
        mstore_slot_t slot = (mstore_slot_t)i;
        mstore_meta_t meta;
        sys_error_t err = mstore_storage_read_meta(st->storage, slot, &meta);
        if (sys_failed(err)) {
            return err;
        }
        if (!meta.used) {
            continue;
        }
        used_count++;

        err = mstore_storage_read_key(st->storage, slot, st->scratch_lookup);
        if (sys_failed(err)) {
            return err;
        }
        mstore_slot_t found;
        err = mstore_index_find(st, st->scratch_lookup, st->scratch_key, &found);
        if (sys_is(err, SYS_CODE_NOT_FOUND) || (sys_ok(err) && found != slot)) {
            return mstore_fail(SYS_CODE_INVARIANT_FAILED);
        }
        if (sys_failed(err)) {
            return err;
        }
        if (st->index_of_slot[slot] == MSTORE_INDEX_NONE) {
            return mstore_fail(SYS_CODE_INVARIANT_FAILED);
        }
    }
    if (used_count != st->live_count) {
        return mstore_fail(SYS_CODE_INVARIANT_FAILED);
    }

    for (size_t i = 0; i < st->index_capacity; i++) {
        mstore_index_entry_t entry = st->index[i];
        if (entry.slot == MSTORE_SLOT_NONE) {
            continue;
        }
        if (entry.slot >= st->schema.capacity) {
            return mstore_fail(SYS_CODE_INVARIANT_FAILED);
        }
        if (st->index_of_slot[entry.slot] != (uint32_t)i) {
            return mstore_fail(SYS_CODE_INVARIANT_FAILED);
        }
        mstore_meta_t meta;
        sys_error_t err = mstore_storage_read_meta(st->storage, entry.slot, &meta);
        if (sys_failed(err)) {
            return err;
        }
        if (!meta.used) {
            return mstore_fail(SYS_CODE_INVARIANT_FAILED);
        }
    }

    if (st->free_count > st->schema.capacity) {
        return mstore_fail(SYS_CODE_INVARIANT_FAILED);
    }
    for (size_t i = 0; i < st->free_count; i++) {
        mstore_slot_t slot = st->free_slots[i];
        if (slot >= st->schema.capacity) {
            return mstore_fail(SYS_CODE_INVARIANT_FAILED);
        }
        mstore_meta_t meta;
        sys_error_t err = mstore_storage_read_meta(st->storage, slot, &meta);
        if (sys_failed(err)) {
            return err;
        }
        if (meta.used) {
            return mstore_fail(SYS_CODE_INVARIANT_FAILED);
        }
        for (size_t j = i + 1; j < st->free_count; j++) {
            if (st->free_slots[j] == slot) {
                return mstore_fail(SYS_CODE_INVARIANT_FAILED);
            }
        }
    }

    if (st->free_count + st->live_count != st->schema.capacity) {
        return mstore_fail(SYS_CODE_INVARIANT_FAILED);
    }
    return SYS_OK;
}
