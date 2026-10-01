#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mstore/mstore_table.h"
#include "mstore_platform.h"
#include "nor_sim.h"
#include "nor_sim_device.h"
#include "storage/mstore_region.h"

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

#define ERASE_SIZE 1024u
#define PARTITION_SIZE 65536u

typedef struct {
    int32_t a;
    int32_t b;
} value_t;

static mstore_table_schema_t schema_for(const char *persist_key, size_t capacity) {
    mstore_table_schema_t schema;
    schema.capacity = capacity;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = MSTORE_BACKING_FLASH;
    schema.persist_key = persist_key;
    return schema;
}

static void fill(mstore_table_t *table, uint32_t key, int32_t value) {
    mstore_slot_t slot;
    uint32_t gen;
    value_t v = {value, value};
    CHECK(sys_ok(mstore_table_slot_allocate(table, &key, &v, &slot, &gen)));
}

static bool read_value(mstore_table_t *table, uint32_t key, int32_t *out) {
    mstore_slot_t found;
    if (sys_failed(mstore_table_slot_find(table, &key, &found))) {
        return false;
    }
    mstore_meta_t meta;
    uint32_t read_key;
    value_t read_value;
    CHECK(sys_ok(mstore_table_slot_read(table, found, &meta, &read_key, &read_value)));
    CHECK(read_key == key);
    *out = read_value.a;
    return true;
}

/* Две таблицы на одном разделе: регионы не пересекаются, данные не портят друг друга. */
static void test_two_tables_share_partition(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(PARTITION_SIZE, ERASE_SIZE);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t alpha = {0};
    mstore_table_t beta = {0};
    mstore_table_schema_t alpha_schema = schema_for("alpha", 2);
    mstore_table_schema_t beta_schema = schema_for("beta", 2);
    CHECK(sys_ok(mstore_table_init(&alpha, &alpha_schema)));
    CHECK(sys_ok(mstore_table_init(&beta, &beta_schema)));

    fill(&alpha, 1, 11);
    fill(&alpha, 2, 12);
    fill(&beta, 1, 21);
    fill(&beta, 2, 22);

    size_t alpha_offset = 0;
    size_t alpha_size = 0;
    size_t beta_offset = 0;
    size_t beta_size = 0;
    CHECK(sys_ok(mstore_region_lookup("alpha", &alpha_offset, &alpha_size)));
    CHECK(sys_ok(mstore_region_lookup("beta", &beta_offset, &beta_size)));
    CHECK(alpha_size == beta_size);
    CHECK(alpha_offset != beta_offset);
    CHECK(alpha_offset >= 2 * ERASE_SIZE);
    CHECK(beta_offset >= 2 * ERASE_SIZE);
    CHECK(alpha_offset + alpha_size <= beta_offset || beta_offset + beta_size <= alpha_offset);

    int32_t value = 0;
    CHECK(read_value(&alpha, 1, &value) && value == 11);
    CHECK(read_value(&alpha, 2, &value) && value == 12);
    CHECK(read_value(&beta, 1, &value) && value == 21);
    CHECK(read_value(&beta, 2, &value) && value == 22);
    CHECK(!read_value(&alpha, 3, &value));

    CHECK(sys_ok(mstore_table_deinit(&alpha)));
    CHECK(sys_ok(mstore_table_deinit(&beta)));

    /* reopen: оба региона находятся в directory на своих местах */
    CHECK(sys_ok(mstore_table_init(&alpha, &alpha_schema)));
    CHECK(sys_ok(mstore_table_init(&beta, &beta_schema)));
    size_t reopen_offset = 0;
    size_t reopen_size = 0;
    CHECK(sys_ok(mstore_region_lookup("alpha", &reopen_offset, &reopen_size)));
    CHECK(reopen_offset == alpha_offset && reopen_size == alpha_size);
    CHECK(read_value(&alpha, 1, &value) && value == 11);
    CHECK(read_value(&beta, 2, &value) && value == 22);

    size_t count = 0;
    CHECK(sys_ok(mstore_table_count(&alpha, &count)) && count == 2);
    CHECK(sys_ok(mstore_table_count(&beta, &count)) && count == 2);
    CHECK(sys_ok(mstore_table_deinit(&alpha)));
    CHECK(sys_ok(mstore_table_deinit(&beta)));

    mstore_nor_sim_destroy(sim);
    printf("test_regions: two tables share partition OK\n");
}

/* Повторный bind без release запрещён: два владельца одного региона не вводим. */
static void test_double_bind_while_bound(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(PARTITION_SIZE, ERASE_SIZE);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_schema_t schema = schema_for("busy", 2);
    mstore_table_t first = {0};
    mstore_table_t second = {0};
    CHECK(sys_ok(mstore_table_init(&first, &schema)));
    CHECK(sys_is(mstore_table_init(&second, &schema), SYS_CODE_INVALID_STATE));

    CHECK(sys_ok(mstore_table_deinit(&first)));
    CHECK(sys_ok(mstore_table_init(&second, &schema)));
    CHECK(sys_ok(mstore_table_deinit(&second)));

    mstore_nor_sim_destroy(sim);
    printf("test_regions: double bind OK\n");
}

/*
 * v1: регион не растёт — смена геометрии диагностируется, а не переразмечает раздел.
 * Различаются два случая:
 *   - новая геометрия влезает в тот же erase-блок: размер региона совпал, расхождение
 *     видит bank header внутри региона и диагностирует backend (INVALID_STATE);
 *   - новой геометрии нужен больший регион: отказывает Region Manager (INVALID_SIZE).
 */
static void test_region_does_not_grow(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(PARTITION_SIZE, ERASE_SIZE);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t table = {0};
    mstore_table_schema_t schema = schema_for("grow", 2);
    CHECK(sys_ok(mstore_table_init(&table, &schema)));
    CHECK(sys_ok(mstore_table_deinit(&table)));

    mstore_table_t same_bank = {0};
    mstore_table_schema_t within_bank = schema_for("grow", 4);
    CHECK(sys_is(mstore_table_init(&same_bank, &within_bank), SYS_CODE_INVALID_STATE));

    mstore_table_t grown = {0};
    mstore_table_schema_t bigger = schema_for("grow", 30);
    CHECK(sys_is(mstore_table_init(&grown, &bigger), SYS_CODE_INVALID_SIZE));

    /* исходная геометрия продолжает открываться */
    CHECK(sys_ok(mstore_table_init(&table, &schema)));
    CHECK(sys_ok(mstore_table_deinit(&table)));

    mstore_nor_sim_destroy(sim);
    printf("test_regions: region does not grow OK\n");
}

/* Directory bounded: места под 16 регионами, 17-й — NO_SPACE. */
static void test_directory_capacity(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(PARTITION_SIZE, ERASE_SIZE);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    static mstore_table_t tables[MSTORE_REGION_MAX_ENTRIES];
    static char keys[MSTORE_REGION_MAX_ENTRIES][8];
    for (size_t i = 0; i < MSTORE_REGION_MAX_ENTRIES; i++) {
        snprintf(keys[i], sizeof(keys[i]), "t%zu", i);
        mstore_table_schema_t schema = schema_for(keys[i], 2);
        CHECK(sys_ok(mstore_table_init(&tables[i], &schema)));
        fill(&tables[i], (uint32_t)i + 1, (int32_t)i + 100);
    }

    mstore_table_t overflow = {0};
    mstore_table_schema_t overflow_schema = schema_for("overflow", 2);
    CHECK(sys_is(mstore_table_init(&overflow, &overflow_schema), SYS_CODE_NO_SPACE));

    for (size_t i = 0; i < MSTORE_REGION_MAX_ENTRIES; i++) {
        int32_t value = 0;
        CHECK(read_value(&tables[i], (uint32_t)i + 1, &value) && value == (int32_t)i + 100);
        CHECK(sys_ok(mstore_table_deinit(&tables[i])));
    }

    mstore_nor_sim_destroy(sim);
    printf("test_regions: directory capacity OK\n");
}

/* Свободного хвоста меньше региона: INVALID_SIZE, а не запись за границей раздела. */
static void test_region_does_not_fit_tail(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(4096, ERASE_SIZE);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_t first = {0};
    mstore_table_schema_t schema = schema_for("first", 2);
    CHECK(sys_ok(mstore_table_init(&first, &schema)));

    mstore_table_t second = {0};
    mstore_table_schema_t other = schema_for("second", 2);
    CHECK(sys_is(mstore_table_init(&second, &other), SYS_CODE_INVALID_SIZE));
    CHECK(sys_ok(mstore_table_deinit(&first)));

    mstore_nor_sim_destroy(sim);
    printf("test_regions: region does not fit tail OK\n");
}

/* Раздел с мусором: валидной directory нет, авто-формат запрещён. */
static void test_dirty_partition_is_corrupt(void) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(PARTITION_SIZE, ERASE_SIZE);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_nor_sim_poke(sim, 0, 0x00);

    mstore_table_t table = {0};
    mstore_table_schema_t schema = schema_for("dirty", 2);
    CHECK(sys_is(mstore_table_init(&table, &schema), SYS_CODE_CORRUPT));

    mstore_nor_sim_destroy(sim);
    printf("test_regions: dirty partition is CORRUPT OK\n");
}

int main(void) {
    test_two_tables_share_partition();
    test_double_bind_while_bound();
    test_region_does_not_grow();
    test_directory_capacity();
    test_region_does_not_fit_tail();
    test_dirty_partition_is_corrupt();
    mstore_platform_flash_set_device(NULL);
    printf("test_regions: OK\n");
    return 0;
}
