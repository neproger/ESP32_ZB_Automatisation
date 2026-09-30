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

#define CAPACITY 4
#define KEYS 8
#define OPS 100000
#define REBOOT_EVERY 5000

typedef struct {
    int32_t a;
    int32_t b;
} value_t;

typedef struct {
    bool used;
    uint32_t key;
    value_t value;
} ref_entry_t;

typedef struct {
    ref_entry_t entries[KEYS];
    size_t live;
} ref_model_t;

typedef struct {
    bool valid;
    mstore_slot_t slot;
    uint32_t generation;
} handle_t;

static uint32_t rng_state = 0x1BADB002u;

static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static ref_entry_t *ref_find(ref_model_t *ref, uint32_t key) {
    for (size_t i = 0; i < KEYS; i++) {
        if (ref->entries[i].used && ref->entries[i].key == key) {
            return &ref->entries[i];
        }
    }
    return NULL;
}

static ref_entry_t *ref_free_entry(ref_model_t *ref) {
    for (size_t i = 0; i < KEYS; i++) {
        if (!ref->entries[i].used) {
            return &ref->entries[i];
        }
    }
    return NULL;
}

static mstore_state_t *state_of(const mstore_table_t *table) {
    return (mstore_state_t *)table->_state;
}

static value_t random_value(void) {
    value_t v = {(int32_t)(rng() % 1000), (int32_t)(rng() % 1000)};
    return v;
}

static void run_op(mstore_table_t *table, ref_model_t *ref, handle_t handles[KEYS]) {
    uint32_t key = rng() % KEYS;
    uint32_t choice = rng() % 100;
    ref_entry_t *entry = ref_find(ref, key);
    mstore_slot_t slot;
    uint32_t generation;
    bool changed;
    value_t dummy = {0, 0};

    if (choice < 55) {
        if (entry != NULL) {
            CHECK(mstore_table_slot_allocate(table, &key, &dummy, &slot, &generation) ==
                  MSTORE_ALREADY_EXISTS);
            return;
        }
        if (ref->live == CAPACITY) {
            CHECK(mstore_table_slot_allocate(table, &key, &dummy, &slot, &generation) ==
                  MSTORE_NO_SPACE);
            return;
        }
        value_t v = random_value();
        CHECK(mstore_table_slot_allocate(table, &key, &v, &slot, &generation) == MSTORE_OK);
        CHECK(slot < CAPACITY);
        ref_entry_t *fresh = ref_free_entry(ref);
        fresh->used = true;
        fresh->key = key;
        fresh->value = v;
        ref->live++;
        handles[key].valid = true;
        handles[key].slot = slot;
        handles[key].generation = generation;
        return;
    }

    if (choice < 80) {
        if (entry == NULL) {
            return;
        }
        value_t v = random_value();
        bool same = v.a == entry->value.a && v.b == entry->value.b;
        changed = true;
        CHECK(mstore_table_slot_update(table, handles[key].slot, handles[key].generation, &v,
                                       &changed) == MSTORE_OK);
        CHECK(changed == !same);
        entry->value = v;
        return;
    }

    if (choice < 95) {
        if (entry == NULL) {
            return;
        }
        CHECK(mstore_table_slot_free(table, handles[key].slot, handles[key].generation) == MSTORE_OK);
        entry->used = false;
        ref->live--;
        handles[key].valid = false;
        return;
    }

    if (entry == NULL) {
        CHECK(mstore_table_slot_find(table, &key, &slot) == MSTORE_NOT_FOUND);
        return;
    }
    CHECK(mstore_table_slot_find(table, &key, &slot) == MSTORE_OK);
    CHECK(slot == handles[key].slot);
    mstore_meta_t meta;
    uint32_t read_key;
    value_t read_value;
    CHECK(mstore_table_slot_read(table, slot, &meta, &read_key, &read_value) == MSTORE_OK);
    CHECK(read_key == key);
    CHECK(read_value.a == entry->value.a && read_value.b == entry->value.b);
}

static void verify_full(mstore_table_t *table, ref_model_t *ref, handle_t handles[KEYS]) {
    size_t count = 0;
    CHECK(mstore_table_count(table, &count) == MSTORE_OK);
    CHECK(count == ref->live);

    for (uint32_t key = 0; key < KEYS; key++) {
        ref_entry_t *entry = ref_find(ref, key);
        mstore_slot_t slot;
        if (entry == NULL) {
            CHECK(mstore_table_slot_find(table, &key, &slot) == MSTORE_NOT_FOUND);
            continue;
        }
        CHECK(mstore_table_slot_find(table, &key, &slot) == MSTORE_OK);
        CHECK(slot == handles[key].slot);
        mstore_meta_t meta;
        uint32_t read_key;
        value_t read_value;
        CHECK(mstore_table_slot_read(table, slot, &meta, &read_key, &read_value) == MSTORE_OK);
        CHECK(read_key == key);
        CHECK(read_value.a == entry->value.a && read_value.b == entry->value.b);
    }
}

static void run_model(mstore_backing_t backing, const char *persist_key) {
    mstore_nor_sim_t *sim = mstore_nor_sim_create(1024, 64);
    CHECK(sim != NULL);
    nor_sim_device_t device;
    nor_sim_device_init(&device, sim);
    mstore_platform_flash_set_device(&device.base);

    mstore_table_schema_t schema;
    schema.capacity = CAPACITY;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = sizeof(value_t);
    schema.payload_equals = NULL;
    schema.backing = backing;
    schema.persist_key = persist_key;

    mstore_table_t table = {0};
    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);

    ref_model_t ref = {0};
    handle_t handles[KEYS] = {0};

    for (int op = 0; op < OPS; op++) {
        run_op(&table, &ref, handles);
        CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);
        if (op % 1000 == 0) {
            verify_full(&table, &ref, handles);
        }
        if (op != 0 && op % REBOOT_EVERY == 0) {
            CHECK(mstore_table_deinit(&table) == MSTORE_OK);
            CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);
            verify_full(&table, &ref, handles);
            CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);
        }
    }

    verify_full(&table, &ref, handles);
    CHECK(mstore_table_deinit(&table) == MSTORE_OK);
    CHECK(mstore_table_init(&table, &schema) == MSTORE_OK);
    verify_full(&table, &ref, handles);
    CHECK(mstore_check_invariants(state_of(&table)) == MSTORE_OK);
    CHECK(mstore_table_deinit(&table) == MSTORE_OK);

    mstore_nor_sim_destroy(sim);
}

int main(void) {
    run_model(MSTORE_BACKING_FLASH, "model_flash");
    printf("test_flash_model: FLASH OK (%d ops)\n", OPS);
    run_model(MSTORE_BACKING_RAM | MSTORE_BACKING_FLASH, "model_composite");
    printf("test_flash_model: RAM|FLASH OK (%d ops)\n", OPS);
    return 0;
}
