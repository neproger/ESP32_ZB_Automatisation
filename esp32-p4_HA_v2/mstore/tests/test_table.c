#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mstore/mstore_table.h"
#include "mstore_internal.h"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

typedef struct {
    int32_t a;
    int32_t b;
} value_t;

static mstore_state_t *state_of(const mstore_table_t *table) {
    return (mstore_state_t *)table->_state;
}

static value_t value(int32_t a, int32_t b) {
    value_t v = {a, b};
    return v;
}

static void test_lifecycle(void) {
    mstore_table_t table = {0};
    const uint32_t keys[5] = {1, 2, 3, 4, 5};
    value_t v = value(10, 20);

    size_t count = 99;
    CHECK(mstore_table_count(&table, &count) == MSTORE_INVALID_STATE);
    CHECK(mstore_table_deinit(&table) == MSTORE_INVALID_STATE);

    mstore_table_schema_t schema;
    schema.capacity = 4;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = MSTORE_BACKING_RAM;
    schema.persist_key = NULL;

    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);
    CHECK(mstore_table_init(&table, &schema) == MSTORE_INVALID_STATE);
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);

    CHECK(mstore_table_count(&table, &count) == MSTORE_OK);
    CHECK(count == 0);

    mstore_slot_t slots[4];
    uint32_t generations[4];
    for (int i = 0; i < 4; i++) {
        CHECK(mstore_table_slot_allocate(&table, &keys[i], &v, &slots[i], &generations[i]) ==
              MSTORE_OK);
        CHECK(generations[i] == 1);
    }
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);

    CHECK(mstore_table_slot_allocate(&table, &keys[0], &v, &slots[0], &generations[0]) ==
          MSTORE_ALREADY_EXISTS);
    CHECK(mstore_table_slot_allocate(&table, &keys[4], &v, &slots[0], &generations[0]) ==
          MSTORE_NO_SPACE);

    mstore_slot_t found = MSTORE_SLOT_NONE;
    CHECK(mstore_table_slot_find(&table, &keys[2], &found) == MSTORE_OK);
    CHECK(found == slots[2]);
    CHECK(mstore_table_slot_find(&table, &keys[4], &found) == MSTORE_NOT_FOUND);

    mstore_meta_t meta;
    uint32_t read_key = 0;
    value_t read_value = value(0, 0);
    CHECK(mstore_table_slot_read(&table, slots[2], &meta, &read_key, &read_value) == MSTORE_OK);
    CHECK(read_key == keys[2]);
    CHECK(read_value.a == v.a && read_value.b == v.b);
    CHECK(meta.version == 1);

    bool changed = true;
    CHECK(mstore_table_slot_update(&table, slots[2], generations[2] + 1, &v, &changed) == MSTORE_STALE);
    CHECK(mstore_table_slot_update(&table, slots[2], generations[2], &v, &changed) == MSTORE_OK);
    CHECK(changed == false);

    value_t v2 = value(30, 40);
    CHECK(mstore_table_slot_update(&table, slots[2], generations[2], &v2, &changed) == MSTORE_OK);
    CHECK(changed == true);
    CHECK(mstore_table_slot_read(&table, slots[2], &meta, &read_key, &read_value) == MSTORE_OK);
    CHECK(read_value.a == v2.a && read_value.b == v2.b);
    CHECK(meta.version == 2);

    CHECK(mstore_table_slot_free(&table, slots[2], generations[2] + 1) == MSTORE_STALE);
    CHECK(mstore_table_slot_free(&table, slots[2], generations[2]) == MSTORE_OK);
    CHECK(mstore_table_slot_find(&table, &keys[2], &found) == MSTORE_NOT_FOUND);
    CHECK(mstore_table_slot_meta(&table, slots[2], &meta) == MSTORE_STALE);
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);

    mstore_slot_t reused = MSTORE_SLOT_NONE;
    uint32_t reused_generation = 0;
    CHECK(mstore_table_slot_allocate(&table, &keys[4], &v, &reused, &reused_generation) == MSTORE_OK);
    CHECK(reused == slots[2]);
    CHECK(reused_generation == generations[2] + 1);
    CHECK(mstore_table_slot_read(&table, reused, &meta, &read_key, &read_value) == MSTORE_OK);
    CHECK(meta.version == 1);

    CHECK(mstore_table_count(&table, &count) == MSTORE_OK);
    CHECK(count == 4);

    CHECK(mstore_table_clear(&table) == MSTORE_OK);
    CHECK(mstore_table_count(&table, &count) == MSTORE_OK);
    CHECK(count == 0);
    CHECK(mstore_table_slot_find(&table, &keys[0], &found) == MSTORE_NOT_FOUND);
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);

    CHECK(mstore_table_deinit(&table) == MSTORE_OK);
    CHECK(mstore_table_deinit(&table) == MSTORE_INVALID_STATE);
}

static bool count_iter_cb(mstore_slot_t slot, const mstore_meta_t *meta, const void *payload,
                          void *ctx) {
    (void)slot;
    (void)meta;
    (void)payload;
    int *seen = ctx;
    (*seen)++;
    return true;
}

static void test_iter(void) {
    mstore_table_t table = {0};
    mstore_table_schema_t schema;
    schema.capacity = 4;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = MSTORE_BACKING_RAM;
    schema.persist_key = NULL;
    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);

    value_t v = value(1, 1);
    for (uint32_t key = 1; key <= 3; key++) {
        mstore_slot_t slot;
        uint32_t generation;
        CHECK(mstore_table_slot_allocate(&table, &key, &v, &slot, &generation) == MSTORE_OK);
    }

    int seen = 0;
    CHECK(mstore_table_iter(&table, count_iter_cb, &seen) == MSTORE_OK);
    CHECK(seen == 3);

    CHECK(mstore_table_deinit(&table) == MSTORE_OK);
}

int main(void) {
    test_lifecycle();
    test_iter();
    printf("test_table: OK\n");
    return 0;
}
