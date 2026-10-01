#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mstore/mstore_ring.h"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

#define CAPACITY 17
#define OPS 300000

typedef struct {
    size_t capacity;
    uint64_t next_seq;
    size_t count;
    int32_t values[CAPACITY];
    uint64_t seqs[CAPACITY];
} ring_ref_t;

static uint32_t rng_state = 0x9e3779b9u;

static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static uint64_t ref_oldest(const ring_ref_t *ref) {
    return ref->next_seq - ref->count;
}

static void ref_append(ring_ref_t *ref, int32_t value, uint64_t seq) {
    if (ref->count < ref->capacity) {
        ref->values[ref->count] = value;
        ref->seqs[ref->count] = seq;
        ref->count++;
        return;
    }
    for (size_t i = 0; i + 1 < ref->count; i++) {
        ref->values[i] = ref->values[i + 1];
        ref->seqs[i] = ref->seqs[i + 1];
    }
    ref->values[ref->count - 1] = value;
    ref->seqs[ref->count - 1] = seq;
}

static bool ref_lookup(const ring_ref_t *ref, uint64_t seq, int32_t *out_value) {
    for (size_t i = 0; i < ref->count; i++) {
        if (ref->seqs[i] == seq) {
            *out_value = ref->values[i];
            return true;
        }
    }
    return false;
}

static uint64_t random_seq(const ring_ref_t *ref) {
    uint32_t mode = rng() % 4;
    if (mode == 0) {
        return 0;
    }
    if (mode == 1) {
        return 1 + rng() % (ref->next_seq + 1);
    }
    if (mode == 2 && ref->count > 0) {
        return ref_oldest(ref) + rng() % ref->count;
    }
    return ref->next_seq + rng() % 3;
}

static void verify_get(const mstore_ring_t *ring, const ring_ref_t *ref, uint64_t seq) {
    int32_t value = 0;
    sys_error_t err = mstore_ring_get_by_seq(ring, seq, &value);

    if (seq == 0 || seq >= ref->next_seq) {
        CHECK(sys_is(err, SYS_CODE_NOT_FOUND));
    } else if (ref->count == 0 || seq < ref_oldest(ref)) {
        CHECK(sys_is(err, SYS_CODE_STALE));
    } else {
        int32_t expected = 0;
        CHECK(ref_lookup(ref, seq, &expected));
        CHECK(sys_ok(err));
        CHECK(value == expected);
    }
}

static void verify_full(const mstore_ring_t *ring, const ring_ref_t *ref) {
    size_t count = 0;
    CHECK(sys_ok(mstore_ring_count(ring, &count)));
    CHECK(count == ref->count);

    uint64_t oldest = 0;
    uint64_t newest = 0;
    if (ref->count == 0) {
        CHECK(sys_is(mstore_ring_oldest_seq(ring, &oldest), SYS_CODE_NOT_FOUND));
        CHECK(sys_is(mstore_ring_newest_seq(ring, &newest), SYS_CODE_NOT_FOUND));
        return;
    }
    CHECK(sys_ok(mstore_ring_oldest_seq(ring, &oldest)));
    CHECK(sys_ok(mstore_ring_newest_seq(ring, &newest)));
    CHECK(oldest == ref_oldest(ref));
    CHECK(newest == ref->next_seq - 1);

    for (uint64_t seq = oldest; seq <= newest; seq++) {
        verify_get(ring, ref, seq);
    }
}

static void run_op(mstore_ring_t *ring, ring_ref_t *ref) {
    uint32_t choice = rng() % 100;

    if (choice < 50) {
        int32_t value = (int32_t)(rng() % 100000);
        uint64_t seq = 0;
        CHECK(sys_ok(mstore_ring_append(ring, &value, &seq)));
        CHECK(seq == ref->next_seq);
        ref_append(ref, value, seq);
        ref->next_seq++;
        return;
    }

    uint64_t seq = random_seq(ref);

    if (choice < 70) {
        verify_get(ring, ref, seq);
        return;
    }

    if (choice < 85) {
        bool contains = true;
        CHECK(sys_ok(mstore_ring_contains(ring, seq, &contains)));
        bool expected = ref->count > 0 && seq >= ref_oldest(ref) && seq < ref->next_seq;
        CHECK(contains == expected);
        return;
    }

    verify_full(ring, ref);
}

int main(void) {
    mstore_ring_t ring = {0};
    mstore_ring_config_t config = {.capacity = CAPACITY, .record_size = sizeof(int32_t)};
    CHECK(sys_ok(mstore_ring_init(&ring, &config)));

    ring_ref_t ref = {.capacity = CAPACITY, .next_seq = 1, .count = 0};

    for (int op = 0; op < OPS; op++) {
        run_op(&ring, &ref);
        if (op % 500 == 0) {
            verify_full(&ring, &ref);
        }
    }

    verify_full(&ring, &ref);
    CHECK(sys_ok(mstore_ring_deinit(&ring)));

    printf("test_ring_model: OK (%d ops)\n", OPS);
    return 0;
}
