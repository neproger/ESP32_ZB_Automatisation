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

static void append_ok(mstore_ring_t *ring, int32_t value, uint64_t expected_seq) {
    uint64_t seq = 0;
    CHECK(mstore_ring_append(ring, &value, &seq) == MSTORE_OK);
    CHECK(seq == expected_seq);
}

static void expect_record(mstore_ring_t *ring, uint64_t seq, int32_t expected) {
    int32_t value = 0;
    CHECK(mstore_ring_get_by_seq(ring, seq, &value) == MSTORE_OK);
    CHECK(value == expected);
}

static void test_bounds(void) {
    mstore_ring_t ring = {0};
    mstore_ring_config_t config = {.capacity = 4, .record_size = sizeof(int32_t)};

    size_t count = 99;
    uint64_t seq = 0;
    bool contains = true;

    CHECK(mstore_ring_count(&ring, &count) == MSTORE_INVALID_STATE);
    CHECK(mstore_ring_deinit(&ring) == MSTORE_INVALID_STATE);

    CHECK(mstore_ring_init(&ring, &config) == MSTORE_OK);
    CHECK(mstore_ring_init(&ring, &config) == MSTORE_INVALID_STATE);

    CHECK(mstore_ring_count(&ring, &count) == MSTORE_OK && count == 0);
    CHECK(mstore_ring_oldest_seq(&ring, &seq) == MSTORE_NOT_FOUND);
    CHECK(mstore_ring_newest_seq(&ring, &seq) == MSTORE_NOT_FOUND);
    CHECK(mstore_ring_contains(&ring, 1, &contains) == MSTORE_OK && contains == false);
    CHECK(mstore_ring_get_by_seq(&ring, 1, &seq) == MSTORE_NOT_FOUND);
    CHECK(mstore_ring_get_by_seq(&ring, 0, &seq) == MSTORE_NOT_FOUND);

    append_ok(&ring, 10, 1);
    append_ok(&ring, 20, 2);
    append_ok(&ring, 30, 3);
    append_ok(&ring, 40, 4);

    CHECK(mstore_ring_count(&ring, &count) == MSTORE_OK && count == 4);
    CHECK(mstore_ring_oldest_seq(&ring, &seq) == MSTORE_OK && seq == 1);
    CHECK(mstore_ring_newest_seq(&ring, &seq) == MSTORE_OK && seq == 4);
    for (uint64_t s = 1; s <= 4; s++) {
        CHECK(mstore_ring_contains(&ring, s, &contains) == MSTORE_OK && contains == true);
    }
    expect_record(&ring, 1, 10);
    expect_record(&ring, 4, 40);

    /* window = [1..4]; append E вытесняет seq 1 */
    append_ok(&ring, 50, 5);

    CHECK(mstore_ring_count(&ring, &count) == MSTORE_OK && count == 4);
    CHECK(mstore_ring_oldest_seq(&ring, &seq) == MSTORE_OK && seq == 2);
    CHECK(mstore_ring_newest_seq(&ring, &seq) == MSTORE_OK && seq == 5);
    CHECK(mstore_ring_get_by_seq(&ring, 1, &seq) == MSTORE_STALE);
    CHECK(mstore_ring_contains(&ring, 1, &contains) == MSTORE_OK && contains == false);
    expect_record(&ring, 2, 20);
    expect_record(&ring, 5, 50);
    CHECK(mstore_ring_get_by_seq(&ring, 6, &seq) == MSTORE_NOT_FOUND);

    /* много полных оборотов */
    int32_t value = 100;
    for (int i = 0; i < 40; i++) {
        uint64_t appended = 0;
        CHECK(mstore_ring_append(&ring, &value, &appended) == MSTORE_OK);
        CHECK(appended == (uint64_t)(6 + i));
    }
    CHECK(mstore_ring_count(&ring, &count) == MSTORE_OK && count == 4);
    CHECK(mstore_ring_oldest_seq(&ring, &seq) == MSTORE_OK && seq == 42);
    CHECK(mstore_ring_newest_seq(&ring, &seq) == MSTORE_OK && seq == 45);
    CHECK(mstore_ring_get_by_seq(&ring, 41, &seq) == MSTORE_STALE);
    expect_record(&ring, 45, 100);

    CHECK(mstore_ring_deinit(&ring) == MSTORE_OK);
    CHECK(mstore_ring_deinit(&ring) == MSTORE_INVALID_STATE);
}

static void test_single_and_exact_capacity(void) {
    mstore_ring_t ring = {0};
    mstore_ring_config_t config = {.capacity = 1, .record_size = sizeof(int32_t)};
    CHECK(mstore_ring_init(&ring, &config) == MSTORE_OK);

    append_ok(&ring, 7, 1);
    expect_record(&ring, 1, 7);

    append_ok(&ring, 8, 2);
    uint64_t seq = 0;
    CHECK(mstore_ring_get_by_seq(&ring, 1, &seq) == MSTORE_STALE);
    expect_record(&ring, 2, 8);

    CHECK(mstore_ring_deinit(&ring) == MSTORE_OK);
}

int main(void) {
    test_bounds();
    test_single_and_exact_capacity();
    printf("test_ring: OK\n");
    return 0;
}
