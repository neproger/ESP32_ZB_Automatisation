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
    CHECK(sys_ok(mstore_ring_append(ring, &value, &seq)));
    CHECK(seq == expected_seq);
}

static void expect_record(mstore_ring_t *ring, uint64_t seq, int32_t expected) {
    int32_t value = 0;
    CHECK(sys_ok(mstore_ring_get_by_seq(ring, seq, &value)));
    CHECK(value == expected);
}

static void test_bounds(void) {
    mstore_ring_t ring = {0};
    mstore_ring_config_t config = {.capacity = 4, .record_size = sizeof(int32_t)};

    size_t count = 99;
    uint64_t seq = 0;
    bool contains = true;

    CHECK(sys_is(mstore_ring_count(&ring, &count), SYS_CODE_INVALID_STATE));
    CHECK(sys_is(mstore_ring_deinit(&ring), SYS_CODE_INVALID_STATE));

    CHECK(sys_ok(mstore_ring_init(&ring, &config)));
    CHECK(sys_is(mstore_ring_init(&ring, &config), SYS_CODE_INVALID_STATE));

    CHECK(sys_ok(mstore_ring_count(&ring, &count)) && count == 0);
    CHECK(sys_is(mstore_ring_oldest_seq(&ring, &seq), SYS_CODE_NOT_FOUND));
    CHECK(sys_is(mstore_ring_newest_seq(&ring, &seq), SYS_CODE_NOT_FOUND));
    CHECK(sys_ok(mstore_ring_contains(&ring, 1, &contains)) && contains == false);
    CHECK(sys_is(mstore_ring_get_by_seq(&ring, 1, &seq), SYS_CODE_NOT_FOUND));
    CHECK(sys_is(mstore_ring_get_by_seq(&ring, 0, &seq), SYS_CODE_NOT_FOUND));

    append_ok(&ring, 10, 1);
    append_ok(&ring, 20, 2);
    append_ok(&ring, 30, 3);
    append_ok(&ring, 40, 4);

    CHECK(sys_ok(mstore_ring_count(&ring, &count)) && count == 4);
    CHECK(sys_ok(mstore_ring_oldest_seq(&ring, &seq)) && seq == 1);
    CHECK(sys_ok(mstore_ring_newest_seq(&ring, &seq)) && seq == 4);
    for (uint64_t s = 1; s <= 4; s++) {
        CHECK(sys_ok(mstore_ring_contains(&ring, s, &contains)) && contains == true);
    }
    expect_record(&ring, 1, 10);
    expect_record(&ring, 4, 40);

    /* window = [1..4]; append E вытесняет seq 1 */
    append_ok(&ring, 50, 5);

    CHECK(sys_ok(mstore_ring_count(&ring, &count)) && count == 4);
    CHECK(sys_ok(mstore_ring_oldest_seq(&ring, &seq)) && seq == 2);
    CHECK(sys_ok(mstore_ring_newest_seq(&ring, &seq)) && seq == 5);
    CHECK(sys_is(mstore_ring_get_by_seq(&ring, 1, &seq), SYS_CODE_STALE));
    CHECK(sys_ok(mstore_ring_contains(&ring, 1, &contains)) && contains == false);
    expect_record(&ring, 2, 20);
    expect_record(&ring, 5, 50);
    CHECK(sys_is(mstore_ring_get_by_seq(&ring, 6, &seq), SYS_CODE_NOT_FOUND));

    /* много полных оборотов */
    int32_t value = 100;
    for (int i = 0; i < 40; i++) {
        uint64_t appended = 0;
        CHECK(sys_ok(mstore_ring_append(&ring, &value, &appended)));
        CHECK(appended == (uint64_t)(6 + i));
    }
    CHECK(sys_ok(mstore_ring_count(&ring, &count)) && count == 4);
    CHECK(sys_ok(mstore_ring_oldest_seq(&ring, &seq)) && seq == 42);
    CHECK(sys_ok(mstore_ring_newest_seq(&ring, &seq)) && seq == 45);
    CHECK(sys_is(mstore_ring_get_by_seq(&ring, 41, &seq), SYS_CODE_STALE));
    expect_record(&ring, 45, 100);

    CHECK(sys_ok(mstore_ring_deinit(&ring)));
    CHECK(sys_is(mstore_ring_deinit(&ring), SYS_CODE_INVALID_STATE));
}

static void test_single_and_exact_capacity(void) {
    mstore_ring_t ring = {0};
    mstore_ring_config_t config = {.capacity = 1, .record_size = sizeof(int32_t)};
    CHECK(sys_ok(mstore_ring_init(&ring, &config)));

    append_ok(&ring, 7, 1);
    expect_record(&ring, 1, 7);

    append_ok(&ring, 8, 2);
    uint64_t seq = 0;
    CHECK(sys_is(mstore_ring_get_by_seq(&ring, 1, &seq), SYS_CODE_STALE));
    expect_record(&ring, 2, 8);

    CHECK(sys_ok(mstore_ring_deinit(&ring)));
}

int main(void) {
    test_bounds();
    test_single_and_exact_capacity();
    printf("test_ring: OK\n");
    return 0;
}
