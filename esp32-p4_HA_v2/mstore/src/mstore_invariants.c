#include "mstore_internal.h"

mstore_err_t mstore_check_invariants(const mstore_state_t *st) {
    if (st == NULL) {
        return MSTORE_INVALID_STATE;
    }

    size_t used_count = 0;
    for (size_t i = 0; i < st->schema.capacity; i++) {
        mstore_slot_t slot = (mstore_slot_t)i;
        if (!mstore_slot_meta(st, slot)->used) {
            continue;
        }
        used_count++;

        mstore_slot_t found;
        if (mstore_index_find(st, mstore_slot_key(st, slot), &found) != MSTORE_OK || found != slot) {
            return MSTORE_INVARIANT_FAILED;
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
        if (slot >= st->schema.capacity || !mstore_slot_meta(st, slot)->used) {
            return MSTORE_INVARIANT_FAILED;
        }
    }

    if (st->free_count > st->schema.capacity) {
        return MSTORE_INVARIANT_FAILED;
    }
    for (size_t i = 0; i < st->free_count; i++) {
        mstore_slot_t slot = st->free_slots[i];
        if (slot >= st->schema.capacity || mstore_slot_meta(st, slot)->used) {
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
