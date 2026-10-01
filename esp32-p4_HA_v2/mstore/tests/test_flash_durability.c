#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mstore/mstore_table.h"
#include "mstore_internal.h"
#include "mstore_platform.h"
#include "nor_sim.h"
#include "nor_sim_device.h"
#include "storage/mstore_region.h"

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

static mstore_state_t *state_of(const mstore_table_t *table) {
    return (mstore_state_t *)table->_state;
}

static mstore_table_schema_t schema_for(const char *persist_key) {
    mstore_table_schema_t schema;
    schema.capacity = 2;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = MSTORE_BACKING_FLASH;
    schema.persist_key = persist_key;
    return schema;
}

static void open_ok(mstore_table_t *table, const char *persist_key) {
    mstore_table_schema_t schema = schema_for(persist_key);
    CHECK(sys_ok(mstore_table_init(table, &schema)));
}

/* Регион таблицы внутри раздела: инъекции бьют по смещениям внутри него. */
static size_t region_base(const char *persist_key) {
    size_t offset = 0;
    size_t size = 0;
    CHECK(sys_ok(mstore_region_lookup(persist_key, &offset, &size)));
    return offset;
}

static void reopen(mstore_table_t *table, const char *persist_key) {
    CHECK(sys_ok(mstore_table_deinit(table)));
    open_ok(table, persist_key);
}

static void test_torn_tail(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    open_ok(&table, "torn");

    value_t v1 = value(1, 1);
    value_t v2 = value(2, 2);
    uint32_t k1 = 1, k2 = 2;
    mstore_slot_t slot;
    uint32_t gen;
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k1, &v1, &slot, &gen)));

    /* power loss посреди append: body оборван */
    mstore_nor_sim_fail_program_after(sim, 3);
    CHECK(sys_is(mstore_table_slot_allocate(&table, &k2, &v2, &slot, &gen), SYS_CODE_IO));

    size_t count = 0;
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 1);

    reopen(&table, "torn");
    CHECK(sys_ok(mstore_table_slot_find(&table, &k1, &slot)));
    CHECK(sys_is(mstore_table_slot_find(&table, &k2, &slot), SYS_CODE_NOT_FOUND));
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 1);
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    /* после torn tail следующий append делает checkpoint и проходит */
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k2, &v2, &slot, &gen)));
    reopen(&table, "torn");
    CHECK(sys_ok(mstore_table_slot_find(&table, &k1, &slot)));
    CHECK(sys_ok(mstore_table_slot_find(&table, &k2, &slot)));
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    CHECK(sys_ok(mstore_table_deinit(&table)));
    mstore_nor_sim_destroy(sim);
    printf("test_flash_durability: torn tail OK\n");
}

static void test_fail_erase_on_clear(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    open_ok(&table, "erase");

    value_t v1 = value(1, 1);
    uint32_t k1 = 1;
    mstore_slot_t slot;
    uint32_t gen;
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k1, &v1, &slot, &gen)));

    mstore_nor_sim_fail_erase(sim);
    CHECK(sys_is(mstore_table_clear(&table), SYS_CODE_IO));

    size_t count = 0;
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 1);

    reopen(&table, "erase");
    CHECK(sys_ok(mstore_table_slot_find(&table, &k1, &slot)));
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    CHECK(sys_ok(mstore_table_deinit(&table)));
    mstore_nor_sim_destroy(sim);
    printf("test_flash_durability: fail erase OK\n");
}

static void test_fail_program_now(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    open_ok(&table, "now");

    value_t v1 = value(1, 1);
    uint32_t k1 = 1;
    mstore_slot_t slot;
    uint32_t gen;
    mstore_nor_sim_fail_program_now(sim);
    CHECK(sys_is(mstore_table_slot_allocate(&table, &k1, &v1, &slot, &gen), SYS_CODE_IO));

    size_t count = 0;
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 0);

    reopen(&table, "now");
    CHECK(sys_ok(mstore_table_count(&table, &count)) && count == 0);
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k1, &v1, &slot, &gen)));
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    CHECK(sys_ok(mstore_table_deinit(&table)));
    mstore_nor_sim_destroy(sim);
    printf("test_flash_durability: fail program now OK\n");
}

static void test_committed_corruption(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    open_ok(&table, "corrupt");

    mstore_slot_t slot;
    uint32_t gen;
    uint32_t k1 = 1;
    value_t v1 = value(1, 1);
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k1, &v1, &slot, &gen)));
    CHECK(sys_ok(mstore_table_deinit(&table)));

    /* active bank 0, header 64, record: payload at 64 + 16 + key_size(4) = 84 */
    mstore_nor_sim_poke(sim, region_base("corrupt") + 84, 0xFF);

    mstore_table_schema_t schema = schema_for("corrupt");
    mstore_table_t reopened = {0};
    CHECK(sys_is(mstore_table_init(&reopened, &schema), SYS_CODE_CORRUPT));

    mstore_nor_sim_destroy(sim);
    printf("test_flash_durability: committed corruption OK\n");
}

static void test_checkpoint_overflow(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    open_ok(&table, "checkpoint");

    uint32_t k0 = 1, k1 = 2;
    mstore_slot_t slot0, slot1;
    uint32_t gen0, gen1;
    value_t v = value(0, 0);
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k0, &v, &slot0, &gen0)));

    bool changed = false;
    /* Аппендов должно хватить на несколько компакшенов: банка capacity 2 с запасом
     * под append вмещает больше двух десятков записей. */
    for (int i = 1; i <= 60; i++) {
        value_t vi = value(i, i);
        CHECK(sys_ok(mstore_table_slot_update(&table, slot0, gen0, &vi, &changed)) && changed);
    }
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k1, &v, &slot1, &gen1)));
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    reopen(&table, "checkpoint");
    mstore_slot_t found;
    mstore_meta_t meta;
    uint32_t rk;
    value_t rv;
    CHECK(sys_ok(mstore_table_slot_find(&table, &k0, &found)));
    CHECK(sys_ok(mstore_table_slot_read(&table, found, &meta, &rk, &rv)));
    CHECK(rk == k0 && rv.a == 60 && rv.b == 60);
    CHECK(sys_ok(mstore_table_slot_find(&table, &k1, &found)));
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    CHECK(sys_ok(mstore_table_deinit(&table)));
    mstore_nor_sim_destroy(sim);
    printf("test_flash_durability: checkpoint overflow OK\n");
}

static void test_header_corruption(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    open_ok(&table, "hdr");

    mstore_slot_t slot;
    uint32_t gen;
    uint32_t k1 = 1;
    value_t v1 = value(1, 1);
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k1, &v1, &slot, &gen)));
    CHECK(sys_ok(mstore_table_deinit(&table)));

    /* повреждаем byte в bank header: region не erased, валидного header нет */
    mstore_nor_sim_poke(sim, region_base("hdr") + 4, 0x00);

    mstore_table_schema_t schema = schema_for("hdr");
    mstore_table_t reopened = {0};
    CHECK(sys_is(mstore_table_init(&reopened, &schema), SYS_CODE_CORRUPT));

    mstore_nor_sim_destroy(sim);
    printf("test_flash_durability: header corruption OK\n");
}

static void test_same_process_torn_recovery(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(8192, 1024);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    open_ok(&table, "torn_same");

    uint32_t k1 = 1, k2 = 2;
    mstore_slot_t slot;
    uint32_t gen;
    value_t v1 = value(1, 1);
    value_t v2 = value(2, 2);
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k1, &v1, &slot, &gen)));

    /* повреждаем append и НЕ перезагружаемся */
    mstore_nor_sim_fail_program_after(sim, 3);
    bool changed = false;
    CHECK(sys_is(mstore_table_slot_update(&table, slot, gen, &v2, &changed), SYS_CODE_IO));

    /* следующая мутация должна сначала сделать checkpoint и пройти */
    mstore_slot_t slot2;
    uint32_t gen2;
    CHECK(sys_ok(mstore_table_slot_allocate(&table, &k2, &v2, &slot2, &gen2)));
    CHECK(sys_ok(mstore_table_slot_find(&table, &k1, &slot)));
    CHECK(sys_ok(mstore_table_slot_find(&table, &k2, &slot)));
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    reopen(&table, "torn_same");
    CHECK(sys_ok(mstore_table_slot_find(&table, &k1, &slot)));
    CHECK(sys_ok(mstore_table_slot_find(&table, &k2, &slot)));
    CHECK(sys_ok(mstore_check_invariants(state_of(&table))));

    CHECK(sys_ok(mstore_table_deinit(&table)));
    mstore_nor_sim_destroy(sim);
    printf("test_flash_durability: same-process torn recovery OK\n");
}

int main(void) {
    test_torn_tail();
    test_fail_erase_on_clear();
    test_fail_program_now();
    test_committed_corruption();
    test_checkpoint_overflow();
    test_header_corruption();
    test_same_process_torn_recovery();
    printf("test_flash_durability: OK\n");
    return 0;
}
