#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "mstore/mstore_ring.h"
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

static void run_table_suite(void) {
    mstore_table_t table = {0};
    mstore_table_schema_t schema = {
        .capacity = 4,
        .key_size = sizeof(uint32_t),
        .payload_size = sizeof(value_t),
        .payload_equals = NULL,
    };

    require(mstore_table_init(&table, &schema) == MSTORE_OK, "table init");

    value_t va = {1, 1};
    value_t vb = {2, 2};
    value_t vc = {3, 3};
    uint32_t key_a = 1, key_b = 2, key_c = 3;
    mstore_slot_t slot_a, slot_b, slot_c;
    uint32_t gen_a, gen_b, gen_c;

    require(mstore_table_slot_allocate(&table, &key_a, &va, &slot_a, &gen_a) == MSTORE_OK, "table alloc A");
    require(mstore_table_slot_allocate(&table, &key_b, &vb, &slot_b, &gen_b) == MSTORE_OK, "table alloc B");

    mstore_slot_t found;
    require(mstore_table_slot_find(&table, &key_a, &found) == MSTORE_OK && found == slot_a, "table find A");

    bool changed = false;
    value_t va2 = {9, 9};
    require(mstore_table_slot_update(&table, slot_a, gen_a, &va2, &changed) == MSTORE_OK && changed,
            "table update A");

    require(mstore_table_slot_free(&table, slot_a, gen_a) == MSTORE_OK, "table free A");
    require(mstore_table_slot_find(&table, &key_a, &found) == MSTORE_NOT_FOUND, "table find freed A");

    require(mstore_table_slot_allocate(&table, &key_c, &vc, &slot_c, &gen_c) == MSTORE_OK, "table alloc C");
    require(slot_c == slot_a, "table slot reuse");
    require(gen_c == gen_a + 1, "table generation bump on reuse");

    size_t count = 0;
    require(mstore_table_count(&table, &count) == MSTORE_OK && count == 2, "table count");

    require(mstore_table_clear(&table) == MSTORE_OK, "table clear");
    require(mstore_table_count(&table, &count) == MSTORE_OK && count == 0, "table count after clear");
    require(mstore_table_deinit(&table) == MSTORE_OK, "table deinit");

    ESP_LOGI(TAG, "table suite: OK");
}

static void run_ring_suite(void) {
    mstore_ring_t ring = {0};
    mstore_ring_config_t config = {.capacity = 4, .record_size = sizeof(int32_t)};
    require(mstore_ring_init(&ring, &config) == MSTORE_OK, "ring init");

    int32_t values[4] = {10, 20, 30, 40};
    for (int i = 0; i < 4; i++) {
        uint64_t seq = 0;
        require(mstore_ring_append(&ring, &values[i], &seq) == MSTORE_OK, "ring append");
        require(seq == (uint64_t)(i + 1), "ring seq");
    }

    int32_t extra = 50;
    uint64_t seq = 0;
    require(mstore_ring_append(&ring, &extra, &seq) == MSTORE_OK && seq == 5, "ring append E");

    uint64_t oldest = 0, newest = 0;
    size_t count = 0;
    require(mstore_ring_oldest_seq(&ring, &oldest) == MSTORE_OK && oldest == 2, "ring oldest");
    require(mstore_ring_newest_seq(&ring, &newest) == MSTORE_OK && newest == 5, "ring newest");
    require(mstore_ring_count(&ring, &count) == MSTORE_OK && count == 4, "ring count");

    int32_t value = 0;
    require(mstore_ring_get_by_seq(&ring, 1, &value) == MSTORE_STALE, "ring stale seq 1");
    require(mstore_ring_get_by_seq(&ring, 2, &value) == MSTORE_OK && value == 20, "ring get 2");
    require(mstore_ring_get_by_seq(&ring, 5, &value) == MSTORE_OK && value == 50, "ring get 5");
    require(mstore_ring_get_by_seq(&ring, 6, &value) == MSTORE_NOT_FOUND, "ring future seq 6");

    bool contains = true;
    require(mstore_ring_contains(&ring, 1, &contains) == MSTORE_OK && !contains, "ring contains 1");
    require(mstore_ring_contains(&ring, 5, &contains) == MSTORE_OK && contains, "ring contains 5");

    for (int i = 0; i < 100; i++) {
        int32_t v = i;
        require(mstore_ring_append(&ring, &v, &seq) == MSTORE_OK, "ring wrap append");
    }
    require(mstore_ring_newest_seq(&ring, &newest) == MSTORE_OK && newest == 105, "ring newest after wraps");
    require(mstore_ring_count(&ring, &count) == MSTORE_OK && count == 4, "ring count after wraps");

    require(mstore_ring_deinit(&ring) == MSTORE_OK, "ring deinit");

    ESP_LOGI(TAG, "ring suite: OK");
}

void app_main(void) {
    run_table_suite();
    run_ring_suite();
    ESP_LOGI(TAG, "ALL MSTORE TESTS PASSED");
}
