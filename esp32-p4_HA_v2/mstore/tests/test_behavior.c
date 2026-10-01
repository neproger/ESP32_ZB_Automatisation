#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mstore/mstore_table.h"
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

static value_t value(int32_t a, int32_t b) {
    value_t v = {a, b};
    return v;
}

typedef struct {
    int seen;
    long long key_sum;
} iter_ctx_t;

static bool count_iter_cb(mstore_slot_t slot, const mstore_meta_t *meta, const void *key,
                          const void *payload, void *ctx) {
    (void)slot;
    (void)meta;
    (void)payload;
    iter_ctx_t *seen = ctx;
    seen->seen++;
    seen->key_sum += *(const uint32_t *)key;
    return true;
}

static void run_suite(mstore_backing_t backing) {
    mstore_table_t table = {0};
    mstore_table_schema_t schema;
    schema.capacity = 4;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = backing;
    schema.persist_key = (backing == MSTORE_BACKING_RAM) ? NULL : "behavior";

    CHECK(sys_ok(mstore_table_init(&table, &schema)));
    CHECK(sys_is(mstore_table_init(&table, &schema), SYS_CODE_INVALID_STATE));

    size_t count = 0;
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 0);

    uint32_t keys[4] = {10, 11, 12, 13};
    mstore_slot_t slots[4];
    uint32_t gens[4];
    value_t v = value(1, 1);
    for (int i = 0; i < 4; i++) {
        CHECK(sys_ok(mstore_table_slot_allocate(&table, &keys[i], &v, &slots[i], &gens[i])));
        CHECK(gens[i] == 1);
    }

    mstore_slot_t tmp;
    CHECK(sys_is(mstore_table_slot_allocate(&table, &keys[0], &v, &tmp, &gens[0]), SYS_CODE_ALREADY_EXISTS));
    uint32_t extra = 99;
    CHECK(sys_is(mstore_table_slot_allocate(&table, &extra, &v, &tmp, &gens[0]), SYS_CODE_NO_SPACE));

    mstore_slot_t found = 0;
    CHECK(sys_ok(mstore_table_slot_find(&table, &keys[2], &found)) && found == slots[2]);
    CHECK(sys_is(mstore_table_slot_find(&table, &extra, &found), SYS_CODE_NOT_FOUND));

    mstore_meta_t meta;
    uint32_t rk;
    value_t rv;
    CHECK(sys_ok(mstore_table_slot_read(&table, slots[2], &meta, &rk, &rv)));
    CHECK(rk == keys[2] && rv.a == 1 && rv.b == 1 && meta.version == 1);

    bool changed = false;
    CHECK(sys_ok(mstore_table_slot_update(&table, slots[2], gens[2], &v, &changed)) && !changed);

    value_t v2 = value(7, 7);
    CHECK(sys_is(mstore_table_slot_update(&table, slots[2], gens[2] + 1, &v2, &changed), SYS_CODE_STALE));
    CHECK(sys_ok(mstore_table_slot_update(&table, slots[2], gens[2], &v2, &changed)) && changed);
    CHECK(sys_ok(mstore_table_slot_read(&table, slots[2], &meta, &rk, &rv)));
    CHECK(rv.a == 7 && rv.b == 7 && meta.version == 2);

    CHECK(sys_is(mstore_table_slot_free(&table, slots[1], gens[1] + 1), SYS_CODE_STALE));
    CHECK(sys_ok(mstore_table_slot_free(&table, slots[1], gens[1])));
    CHECK(sys_is(mstore_table_slot_find(&table, &keys[1], &found), SYS_CODE_NOT_FOUND));
    CHECK(sys_is(mstore_table_slot_meta(&table, slots[1], &meta), SYS_CODE_STALE));

    mstore_slot_t reused;
    uint32_t reused_gen;
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &extra, &v, &reused, &reused_gen)));
    CHECK(reused == slots[1] && reused_gen == gens[1] + 1);

    /* после free key=11 и переиспользования слота под extra=99: 10+12+13+99 == 134 */
    iter_ctx_t seen = {0, 0};
    CHECK(sys_ok(mstore_table_iter(&table, count_iter_cb, &seen)) && seen.seen == 4);
    CHECK(seen.key_sum == 134);
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 4);

    CHECK(sys_ok(mstore_table_clear(&table)));
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 0);

    CHECK(sys_ok(mstore_table_deinit(&table)));
    CHECK(sys_is(mstore_table_deinit(&table), SYS_CODE_INVALID_STATE));
}

static void run_ram(void) {
    run_suite(MSTORE_BACKING_RAM);
    printf("test_behavior: RAM OK\n");
}

static void run_flash(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(4096, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    run_suite(MSTORE_BACKING_FLASH);

    mstore_nor_sim_destroy(sim);
    printf("test_behavior: FLASH OK\n");
}

static void run_composite(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(4096, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    run_suite(MSTORE_BACKING_RAM | MSTORE_BACKING_FLASH);

    mstore_nor_sim_destroy(sim);
    printf("test_behavior: RAM|FLASH OK\n");
}

int main(void) {
    run_ram();
    run_flash();
    run_composite();
    printf("test_behavior: OK\n");
    return 0;
}
