#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "mstore/mstore_table.h"

static const char *TAG = "mstore_test";

typedef struct {
    int32_t a;
    int32_t b;
} value_t;

static void require(bool condition, const char *what) {
    if (!condition) {
        ESP_LOGE(TAG, "FAIL: %s", what);
        abort();
    }
}

static void run_lifecycle(void) {
    mstore_table_t table = {0};
    mstore_table_schema_t schema = {
        .capacity = 4,
        .key_size = sizeof(uint32_t),
        .payload_size = sizeof(value_t),
        .payload_equals = NULL,
    };

    require(mstore_table_init(&table, &schema) == MSTORE_OK, "init");

    value_t va = {1, 1};
    value_t vb = {2, 2};
    value_t vc = {3, 3};
    uint32_t key_a = 1, key_b = 2, key_c = 3;
    mstore_slot_t slot_a, slot_b, slot_c;
    uint32_t gen_a, gen_b, gen_c;

    require(mstore_table_slot_allocate(&table, &key_a, &va, &slot_a, &gen_a) == MSTORE_OK, "alloc A");
    require(mstore_table_slot_allocate(&table, &key_b, &vb, &slot_b, &gen_b) == MSTORE_OK, "alloc B");

    mstore_slot_t found;
    require(mstore_table_slot_find(&table, &key_a, &found) == MSTORE_OK && found == slot_a, "find A");

    bool changed = false;
    value_t va2 = {9, 9};
    require(mstore_table_slot_update(&table, slot_a, gen_a, &va2, &changed) == MSTORE_OK && changed,
            "update A");

    require(mstore_table_slot_free(&table, slot_a, gen_a) == MSTORE_OK, "free A");
    require(mstore_table_slot_find(&table, &key_a, &found) == MSTORE_NOT_FOUND, "find freed A");

    require(mstore_table_slot_allocate(&table, &key_c, &vc, &slot_c, &gen_c) == MSTORE_OK, "alloc C");
    require(slot_c == slot_a, "slot reuse");
    require(gen_c == gen_a + 1, "generation bump on reuse");

    size_t count = 0;
    require(mstore_table_count(&table, &count) == MSTORE_OK && count == 2, "count");

    require(mstore_table_clear(&table) == MSTORE_OK, "clear");
    require(mstore_table_count(&table, &count) == MSTORE_OK && count == 0, "count after clear");
    require(mstore_table_deinit(&table) == MSTORE_OK, "deinit");

    ESP_LOGI(TAG, "mstore p4 lifecycle: OK");
}

void app_main(void) {
    run_lifecycle();
}
