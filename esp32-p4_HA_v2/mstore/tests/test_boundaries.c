#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

static void write_slot_meta(mstore_table_t *table, mstore_slot_t slot, mstore_meta_t meta) {
    CHECK(mstore_storage_write_meta(state_of(table)->storage, slot, &meta) == MSTORE_OK);
}

static void test_generation_overflow(void) {
    mstore_table_t table = {0};
    mstore_table_schema_t schema;
    schema.capacity = 1;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = MSTORE_BACKING_RAM;
    schema.persist_key = NULL;
    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);

    uint32_t key = 1;
    value_t v = {1, 1};
    mstore_slot_t slot;
    uint32_t gen;

    /* slot на пределе generation -> новый allocate не переиспользует его */
    mstore_meta_t meta = {false, UINT32_MAX, 0};
    write_slot_meta(&table, 0, meta);
    CHECK(mstore_table_slot_allocate(&table, &key, &v, &slot, &gen) == MSTORE_OVERFLOW);

    /* generation = MAX-1 -> allocate доводит до MAX, следующий reuse -> OVERFLOW */
    meta.generation = UINT32_MAX - 1;
    write_slot_meta(&table, 0, meta);
    CHECK(mstore_table_slot_allocate(&table, &key, &v, &slot, &gen) == MSTORE_OK);
    CHECK(gen == UINT32_MAX);
    CHECK(mstore_table_slot_free(&table, slot, gen) == MSTORE_OK);
    CHECK(mstore_table_slot_allocate(&table, &key, &v, &slot, &gen) == MSTORE_OVERFLOW);

    CHECK(mstore_table_deinit(&table) == MSTORE_OK);
    printf("test_boundaries: generation overflow OK\n");
}

static void test_version_overflow(void) {
    mstore_table_t table = {0};
    mstore_table_schema_t schema;
    schema.capacity = 1;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = MSTORE_BACKING_RAM;
    schema.persist_key = NULL;
    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);

    uint32_t key = 1;
    value_t v = {1, 1};
    mstore_slot_t slot;
    uint32_t gen;
    CHECK(mstore_table_slot_allocate(&table, &key, &v, &slot, &gen) == MSTORE_OK);

    /* version на пределе: реальное изменение -> OVERFLOW, unchanged -> OK */
    mstore_meta_t meta = {true, gen, UINT32_MAX};
    write_slot_meta(&table, slot, meta);

    bool changed = true;
    CHECK(mstore_table_slot_update(&table, slot, gen, &v, &changed) == MSTORE_OK && !changed);

    value_t v2 = {2, 2};
    CHECK(mstore_table_slot_update(&table, slot, gen, &v2, &changed) == MSTORE_OVERFLOW);

    CHECK(mstore_table_deinit(&table) == MSTORE_OK);
    printf("test_boundaries: version overflow OK\n");
}

int main(void) {
    test_generation_overflow();
    test_version_overflow();
    printf("test_boundaries: OK\n");
    return 0;
}
