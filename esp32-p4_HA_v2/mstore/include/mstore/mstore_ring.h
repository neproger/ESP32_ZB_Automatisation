#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mstore/mstore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ring Store: bounded окно последовательных записей (append / overwrite-oldest).
 * seq — долговечная логическая identity записи; физический slot — внутренняя деталь.
 * Index / generation / free-list у Ring нет.
 */

typedef struct {
    void *_state;
} mstore_ring_t;

typedef struct {
    size_t capacity;
    size_t record_size;
} mstore_ring_config_t;

mstore_err_t mstore_ring_init(mstore_ring_t *ring, const mstore_ring_config_t *config);
mstore_err_t mstore_ring_deinit(mstore_ring_t *ring);

mstore_err_t mstore_ring_append(mstore_ring_t *ring, const void *record, uint64_t *out_seq);
mstore_err_t mstore_ring_get_by_seq(const mstore_ring_t *ring, uint64_t seq, void *out_record);

mstore_err_t mstore_ring_oldest_seq(const mstore_ring_t *ring, uint64_t *out_seq);
mstore_err_t mstore_ring_newest_seq(const mstore_ring_t *ring, uint64_t *out_seq);
mstore_err_t mstore_ring_contains(const mstore_ring_t *ring, uint64_t seq, bool *out_contains);
mstore_err_t mstore_ring_count(const mstore_ring_t *ring, size_t *out_count);

#ifdef __cplusplus
}
#endif
