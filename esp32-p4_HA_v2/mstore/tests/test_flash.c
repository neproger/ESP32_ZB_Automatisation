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

static mstore_table_schema_t make_schema(void) {
    mstore_table_schema_t schema;
    schema.capacity = 8;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = MSTORE_BACKING_FLASH;
    schema.persist_key = "test_table";
    return schema;
}

static void open_table(mstore_table_t *table) {
    mstore_table_schema_t schema = make_schema();
    CHECK(sys_ok(mstore_table_init(table, &schema)));
}

/* Открывает таблицу заданной ёмкости на отдельной NOR-области. */
static bool try_open_capacity(size_t capacity) {
    mstore_nor_sim_t *probe_sim = mstore_nor_sim_create(8192, 1024);
    nor_sim_device_t probe_device;
    nor_sim_device_init(&probe_device, probe_sim);
    mstore_platform_flash_set_device(&probe_device.base);

    mstore_table_t table = {0};
    mstore_table_schema_t schema = make_schema();
    schema.capacity = capacity;
    bool opened = sys_ok(mstore_table_init(&table, &schema));
    if (opened) {
        CHECK(sys_ok(mstore_table_deinit(&table)));
    }

    mstore_platform_flash_set_device(NULL);
    mstore_nor_sim_destroy(probe_sim);
    return opened;
}

/* Банка обязана резервировать место под append: иначе при полностью занятой
 * таблице update существующего слота получал NO_SPACE вместо OK. */
static void test_bank_reserves_append_headroom(void) {
    size_t largest = 0;
    for (size_t capacity = 1; capacity <= 4096; capacity++) {
        if (!try_open_capacity(capacity)) {
            break;
        }
        largest = capacity;
    }
    CHECK(largest > 0 && largest < 4096);

    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    mstore_table_schema_t schema = make_schema();
    schema.capacity = largest;
    CHECK(sys_ok(mstore_table_init(&table, &schema)));

    static uint32_t keys[4096];
    static mstore_slot_t slots[4096];
    static uint32_t generations[4096];
    for (size_t i = 0; i < largest; i++) {
        keys[i] = (uint32_t)i + 1;
        value_t v = {1, 1};
        CHECK(sys_ok(mstore_table_slot_allocate(&table, &keys[i], &v, &slots[i], &generations[i])));
    }

    size_t count = 0;
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == largest);

    for (size_t i = 0; i < largest; i++) {
        value_t updated = {(int32_t)i + 100, (int32_t)i + 200};
        bool changed = false;
        CHECK(sys_ok(mstore_table_slot_update(&table, slots[i], generations[i], &updated, &changed)));
        CHECK(changed);
    }

    CHECK(sys_ok(mstore_table_deinit(&table)));
    mstore_platform_flash_set_device(NULL);
    mstore_nor_sim_destroy(sim);
}

int main(void) {
    test_bank_reserves_append_headroom(); /* работает на отдельной NOR-области */

    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    open_table(&table);

    value_t v1 = {1, 1};
    value_t v2 = {2, 2};
    uint32_t key1 = 1, key2 = 2;
    mstore_slot_t slot1, slot2;
    uint32_t gen1, gen2;

    CHECK(sys_ok(mstore_table_slot_allocate(&table, &key1, &v1, &slot1, &gen1)));
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &key2, &v2, &slot2, &gen2)));
    CHECK(sys_is(mstore_table_slot_allocate(&table, &key1, &v1, &slot1, &gen1), SYS_CODE_ALREADY_EXISTS));

    mstore_slot_t found = 0;
    CHECK(sys_ok(mstore_table_slot_find(&table, &key1, &found)) && found == slot1);

    bool changed = false;
    value_t v1b = {11, 11};
    CHECK(sys_ok(mstore_table_slot_update(&table, slot1, gen1, &v1b, &changed)) && changed);
    CHECK(sys_ok(mstore_table_slot_update(&table, slot1, gen1, &v1b, &changed)) && !changed);

    size_t count = 0;
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 2);
    CHECK(sys_ok(mstore_table_deinit(&table)));

    /* reboot: та же flash-область, новая table */
    open_table(&table);
    CHECK(sys_ok(mstore_table_slot_find(&table, &key1, &found)));
    mstore_meta_t meta;
    uint32_t rk;
    value_t rv;
    CHECK(sys_ok(mstore_table_slot_read(&table, found, &meta, &rk, &rv)));
    CHECK(rk == key1 && rv.a == v1b.a && rv.b == v1b.b && meta.version == 2);

    CHECK(sys_ok(mstore_table_slot_free(&table, found, gen1)));
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 1);
    CHECK(sys_ok(mstore_table_deinit(&table)));

    /* reboot после free */
    open_table(&table);
    CHECK(sys_is(mstore_table_slot_find(&table, &key1, &found), SYS_CODE_NOT_FOUND));
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 1);
    CHECK(sys_ok(mstore_table_slot_find(&table, &key2, &found)));

    CHECK(sys_ok(mstore_table_clear(&table)));
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 0);
    CHECK(sys_ok(mstore_table_deinit(&table)));

    /* reboot после clear */
    open_table(&table);
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 0);
    CHECK(sys_is(mstore_table_slot_find(&table, &key2, &found), SYS_CODE_NOT_FOUND));
    CHECK(sys_ok(mstore_table_deinit(&table)));

    mstore_nor_sim_destroy(sim);
    printf("test_flash: OK\n");
    return 0;
}
