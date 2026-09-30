#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mstore/mstore_types.h"
#include "mstore_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSTORE_SLOT_NONE UINT32_MAX

typedef struct {
    size_t capacity;
    size_t key_size;
    size_t payload_size;
    size_t slot_size; /* sizeof(meta) + key_size + payload_size, выровнен под meta */
    mstore_payload_equals_fn payload_equals;
} mstore_schema_t;

/*
 * Canonical state — slots. index/free-list/live_count — производные и
 * полностью восстанавливаются из slots через mstore_runtime_rebuild.
 */
typedef struct {
    mstore_schema_t schema;

    uint8_t *slots;            /* capacity * slot_size */
    mstore_slot_t *free_slots; /* LIFO, capacity entries */
    size_t free_count;
    size_t live_count;

    mstore_slot_t *index;  /* open-addressed key -> slot, MSTORE_SLOT_NONE = пусто */
    size_t index_capacity; /* power of two, >= 2 * capacity */

    void *lock;
} mstore_state_t;

static inline const mstore_meta_t *mstore_slot_meta(const mstore_state_t *st, mstore_slot_t slot) {
    return (const mstore_meta_t *)(st->slots + (size_t)slot * st->schema.slot_size);
}

static inline mstore_meta_t *mstore_slot_meta_mut(mstore_state_t *st, mstore_slot_t slot) {
    return (mstore_meta_t *)(st->slots + (size_t)slot * st->schema.slot_size);
}

static inline const void *mstore_slot_key(const mstore_state_t *st, mstore_slot_t slot) {
    return st->slots + (size_t)slot * st->schema.slot_size + sizeof(mstore_meta_t);
}

static inline void *mstore_slot_key_mut(mstore_state_t *st, mstore_slot_t slot) {
    return st->slots + (size_t)slot * st->schema.slot_size + sizeof(mstore_meta_t);
}

static inline const void *mstore_slot_payload(const mstore_state_t *st, mstore_slot_t slot) {
    return (const uint8_t *)mstore_slot_key(st, slot) + st->schema.key_size;
}

static inline void *mstore_slot_payload_mut(mstore_state_t *st, mstore_slot_t slot) {
    return (uint8_t *)mstore_slot_key_mut(st, slot) + st->schema.key_size;
}

/* runtime.c — derived acceleration, полностью rebuildable */
mstore_err_t mstore_runtime_rebuild(mstore_state_t *st);
mstore_err_t mstore_index_find(const mstore_state_t *st, const void *key, mstore_slot_t *out_slot);
void mstore_index_insert(mstore_state_t *st, mstore_slot_t slot);
void mstore_index_remove(mstore_state_t *st, mstore_slot_t slot);
mstore_err_t mstore_freelist_pop(mstore_state_t *st, mstore_slot_t *out_slot);
void mstore_freelist_push(mstore_state_t *st, mstore_slot_t slot);

/* invariants.c — диагностика корректности */
mstore_err_t mstore_check_invariants(const mstore_state_t *st);

#ifdef __cplusplus
}
#endif
