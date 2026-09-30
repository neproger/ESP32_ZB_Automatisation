#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mstore/mstore_types.h"
#include "mstore_platform.h"
#include "storage/mstore_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSTORE_SLOT_NONE UINT32_MAX
#define MSTORE_INDEX_NONE UINT32_MAX

typedef struct {
    size_t capacity;
    size_t key_size;
    size_t payload_size;
    mstore_payload_equals_fn payload_equals;
} mstore_schema_t;

/*
 * Индекс хранит вместе со slot его home bucket. Это позволяет backward-shift
 * deletion без чтения key из backend: post-commit derived update остаётся
 * infallible и не зависит от I/O.
 */
typedef struct {
    mstore_slot_t slot;
    uint32_t home;
} mstore_index_entry_t;

/*
 * Table Engine state.
 * canonical slot state живёт в storage backend; здесь — derived runtime
 * (index / free-list / live_count) и scratch под lock.
 */
typedef struct {
    mstore_schema_t schema;
    mstore_backing_t backing;
    mstore_storage_t *storage;

    mstore_slot_t *free_slots; /* LIFO, capacity entries */
    size_t free_count;
    size_t live_count;

    mstore_index_entry_t *index; /* open-addressed; MSTORE_SLOT_NONE = пусто */
    uint32_t *index_of_slot;     /* slot -> bucket, MSTORE_INDEX_NONE = нет */
    size_t index_capacity;       /* power of two, >= 2 * capacity */

    void *scratch_key;     /* key_size bytes; валиден только под lock */
    void *scratch_payload; /* payload_size bytes; валиден только под lock */

    void *lock;
} mstore_state_t;

/* runtime.c — derived acceleration */
void mstore_runtime_reset(mstore_state_t *st);   /* all slots free, без I/O */
mstore_err_t mstore_runtime_rebuild(mstore_state_t *st); /* из storage */
mstore_err_t mstore_index_find(mstore_state_t *st, const void *key, mstore_slot_t *out_slot);
void mstore_index_insert(mstore_state_t *st, mstore_slot_t slot, const void *key);
void mstore_index_remove(mstore_state_t *st, mstore_slot_t slot);
mstore_err_t mstore_freelist_pop(mstore_state_t *st, mstore_slot_t *out_slot);
void mstore_freelist_push(mstore_state_t *st, mstore_slot_t slot);

/* invariants.c — диагностика корректности; отличает нарушение от ошибки storage */
mstore_err_t mstore_check_invariants(mstore_state_t *st);

#ifdef __cplusplus
}
#endif
