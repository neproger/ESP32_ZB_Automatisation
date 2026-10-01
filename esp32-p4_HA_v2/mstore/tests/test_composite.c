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
    CHECK(sys_ok(mstore_table_init(&table, &schema)));

    uint32_t k1 = 1;
    mstore_slot_t slot;
    uint32_t gen;
    value_t v1 = {1, 1};
    value_t v2 = {2, 2};
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k1, &v1, &slot, &gen)));
    bool changed = false;
    CHECK(sys_ok(mstore_table_slot_update(&table, slot, gen, &v2, &changed)) && changed);
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    /* reboot: состояние восстанавливается из durable FLASH */
    CHECK(sys_ok(mstore_table_deinit(&table)));
    CHECK(sys_ok(mstore_table_init(&table, &schema)));
    mstore_meta_t meta;
    uint32_t rk;
    value_t rv;
    mstore_slot_t found;
    CHECK(sys_ok(mstore_table_slot_find(&table, &k1, &found)));
    CHECK(sys_ok(mstore_table_slot_read(&table, found, &meta, &rk, &rv)));
    CHECK(rv.a == v2.a && rv.b == v2.b);
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    /* FLASH write падает: RAM не меняется (write-through) */
    value_t v3 = {3, 3};
    mstore_nor_sim_fail_program_after(sim, 3);
    CHECK(sys_is(mstore_table_slot_update(&table, found, gen, &v3, &changed), SYS_CODE_IO));
    CHECK(sys_ok(mstore_table_slot_read(&table, found, &meta, &rk, &rv)));
    CHECK(rv.a == v2.a && rv.b == v2.b);
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    /* reopen: durable FLASH тоже остался на v2 */
    CHECK(sys_ok(mstore_table_deinit(&table)));
    CHECK(sys_ok(mstore_table_init(&table, &schema)));
    CHECK(sys_ok(mstore_table_slot_find(&table, &k1, &found)));
    CHECK(sys_ok(mstore_table_slot_read(&table, found, &meta, &rk, &rv)));
    CHECK(rv.a == v2.a && rv.b == v2.b);
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    size_t count = 0;
    CHECK(sys_ok(mstore_table_clear(&table)));
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 0);
    CHECK(sys_ok(mstore_table_deinit(&table)));
    CHECK(sys_ok(mstore_table_init(&table, &schema)));
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 0);
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));
    CHECK(sys_ok(mstore_table_deinit(&table)));

    mstore_nor_sim_destroy(sim);
    printf("test_composite: OK\n");
    return 0;
}
