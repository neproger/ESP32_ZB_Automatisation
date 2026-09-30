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

static void require_err(mstore_err_t err, const char *what) {
    if (err != MSTORE_OK) {
        ESP_LOGE(TAG, "FAIL: %s err=%d", what, (int)err);
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
        .backing = MSTORE_BACKING_RAM,
        .persist_key = NULL,
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

static void run_flash_suite(void) {
    mstore_table_t table = {0};
    mstore_table_schema_t schema = {
        .capacity = 8,
        .key_size = sizeof(uint32_t),
        .payload_size = sizeof(value_t),
        .payload_equals = NULL,
        .backing = MSTORE_BACKING_FLASH,
        .persist_key = "p4_flash",
    };

    require(mstore_table_init(&table, &schema) == MSTORE_OK, "flash init");
    require(mstore_table_clear(&table) == MSTORE_OK, "flash clear");

    uint32_t key = 7;
    value_t v = {7, 7};
    mstore_slot_t slot;
    uint32_t gen;
    require(mstore_table_slot_allocate(&table, &key, &v, &slot, &gen) == MSTORE_OK, "flash alloc");
    require(mstore_table_deinit(&table) == MSTORE_OK, "flash deinit");

    /* reboot: тот же durable partition */
    mstore_table_t reopened = {0};
    require(mstore_table_init(&reopened, &schema) == MSTORE_OK, "flash reopen");
    mstore_slot_t found;
    require(mstore_table_slot_find(&reopened, &key, &found) == MSTORE_OK, "flash find after reboot");
    mstore_meta_t meta;
    uint32_t read_key;
    value_t read_value;
    require(mstore_table_slot_read(&reopened, found, &meta, &read_key, &read_value) == MSTORE_OK,
            "flash read");
    require(read_key == key && read_value.a == 7 && read_value.b == 7, "flash payload");
    require(mstore_table_deinit(&reopened) == MSTORE_OK, "flash deinit 2");

    ESP_LOGI(TAG, "flash suite: OK");
}

/* Две FLASH-таблицы на одном разделе: регионы не пересекаются, reboot находит оба. */
static void run_region_suite(void) {
    mstore_table_schema_t schema_a = {
        .capacity = 4,
        .key_size = sizeof(uint32_t),
        .payload_size = sizeof(value_t),
        .payload_equals = NULL,
        .backing = MSTORE_BACKING_FLASH,
        .persist_key = "p4_region_a",
    };
    mstore_table_schema_t schema_b = schema_a;
    schema_b.persist_key = "p4_region_b";

    mstore_table_t a = {0};
    mstore_table_t b = {0};
    require_err(mstore_table_init(&a, &schema_a), "region A init");
    require_err(mstore_table_init(&b, &schema_b), "region B init");
    require_err(mstore_table_clear(&a), "region A clear");
    require_err(mstore_table_clear(&b), "region B clear");

    uint32_t key_a = 100;
    uint32_t key_b = 200;
    value_t va = {7, 8};
    value_t vb = {9, 10};
    mstore_slot_t slot_a;
    mstore_slot_t slot_b;
    uint32_t gen_a;
    uint32_t gen_b;
    require_err(mstore_table_slot_allocate(&a, &key_a, &va, &slot_a, &gen_a), "region A alloc");
    require_err(mstore_table_slot_allocate(&b, &key_b, &vb, &slot_b, &gen_b), "region B alloc");

    size_t count_a = 0;
    size_t count_b = 0;
    require_err(mstore_table_count(&a, &count_a), "region A count");
    require_err(mstore_table_count(&b, &count_b), "region B count");
    require(count_a == 1 && count_b == 1, "regions independent");

    require_err(mstore_table_deinit(&a), "region A deinit");
    require_err(mstore_table_deinit(&b), "region B deinit");

    /* reopen: оба региона остаются на своих местах в directory */
    mstore_table_t reopened_a = {0};
    mstore_table_t reopened_b = {0};
    require_err(mstore_table_init(&reopened_a, &schema_a), "region A reopen");
    require_err(mstore_table_init(&reopened_b, &schema_b), "region B reopen");

    mstore_slot_t found;
    mstore_meta_t meta;
    uint32_t read_key;
    value_t read_value;
    require_err(mstore_table_slot_find(&reopened_a, &key_a, &found), "region A find");
    require_err(mstore_table_slot_read(&reopened_a, found, &meta, &read_key, &read_value),
                "region A read");
    require(read_key == key_a && read_value.a == 7 && read_value.b == 8, "region A payload");

    require_err(mstore_table_slot_find(&reopened_b, &key_b, &found), "region B find");
    require_err(mstore_table_slot_read(&reopened_b, found, &meta, &read_key, &read_value),
                "region B read");
    require(read_key == key_b && read_value.a == 9 && read_value.b == 10, "region B payload");

    require_err(mstore_table_deinit(&reopened_a), "region A deinit 2");
    require_err(mstore_table_deinit(&reopened_b), "region B deinit 2");

    ESP_LOGI(TAG, "region suite: OK");
}

void app_main(void) {
    run_table_suite();
    run_ring_suite();
    run_flash_suite();
    run_region_suite();
    ESP_LOGI(TAG, "ALL MSTORE TESTS PASSED");
}
