#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mstore/mstore_table.h"
#include "mstore_internal.h"
#include "mstore_platform.h"
#include "nor_sim.h"
#include "nor_sim_device.h"

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

static mstore_table_schema_t make_schema(void) {
    mstore_table_schema_t schema;
    schema.capacity = 2;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = MSTORE_BACKING_RAM | MSTORE_BACKING_FLASH;
    schema.persist_key = "composite";
    return schema;
}

int main(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_schema_t schema = make_schema();
    mstore_table_t table = {0};
    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);

    uint32_t k1 = 1;
    mstore_slot_t slot;
    uint32_t gen;
    value_t v1 = {1, 1};
    value_t v2 = {2, 2};
    CHECK(mstore_table_slot_allocate(&table, &k1, &v1, &slot, &gen) == MSTORE_OK);
    bool changed = false;
    CHECK(mstore_table_slot_update(&table, slot, gen, &v2, &changed) == MSTORE_OK && changed);
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);

    /* reboot: состояние восстанавливается из durable FLASH */
    CHECK(mstore_table_deinit(&table) == MSTORE_OK);
    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);
    mstore_meta_t meta;
    uint32_t rk;
    value_t rv;
    mstore_slot_t found;
    CHECK(mstore_table_slot_find(&table, &k1, &found) == MSTORE_OK);
    CHECK(mstore_table_slot_read(&table, found, &meta, &rk, &rv) == MSTORE_OK);
    CHECK(rv.a == v2.a && rv.b == v2.b);
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);

    /* FLASH write падает: RAM не меняется (write-through) */
    value_t v3 = {3, 3};
    mstore_nor_sim_fail_program_after(sim, 3);
    CHECK(mstore_table_slot_update(&table, found, gen, &v3, &changed) == MSTORE_IO);
    CHECK(mstore_table_slot_read(&table, found, &meta, &rk, &rv) == MSTORE_OK);
    CHECK(rv.a == v2.a && rv.b == v2.b);
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);

    /* reopen: durable FLASH тоже остался на v2 */
    CHECK(mstore_table_deinit(&table) == MSTORE_OK);
    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);
    CHECK(mstore_table_slot_find(&table, &k1, &found) == MSTORE_OK);
    CHECK(mstore_table_slot_read(&table, found, &meta, &rk, &rv) == MSTORE_OK);
    CHECK(rv.a == v2.a && rv.b == v2.b);
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);

    size_t count = 0;
    CHECK(mstore_table_clear(&table) == MSTORE_OK);
    CHECK(mstore_table_count(&table, &count) == MSTORE_OK && count == 0);
    CHECK(mstore_table_deinit(&table) == MSTORE_OK);
    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);
    CHECK(mstore_table_count(&table, &count) == MSTORE_OK && count == 0);
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);
    CHECK(mstore_table_deinit(&table) == MSTORE_OK);

    mstore_nor_sim_destroy(sim);
    printf("test_composite: OK\n");
    return 0;
}
