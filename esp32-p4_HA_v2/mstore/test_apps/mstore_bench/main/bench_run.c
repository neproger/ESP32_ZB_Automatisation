#include "bench_run.h"

#include <stdio.h>
#include <string.h>

#include "bench_stats.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mstore/mstore_bench.h"
#include "mstore/mstore_table.h"

#define BENCH_PAYLOAD_SIZE 32u
#define BENCH_MAX_CAPACITY 5000u
#define BENCH_TASK_PRIORITY 5
#define BENCH_ITER_ROUNDS 10u /* iter читает key+payload каждого слота: на 5000 слотов это ~1 с за проход */

typedef union {
    uint8_t bytes[BENCH_PAYLOAD_SIZE];
    struct {
        uint32_t a;
        uint32_t b;
    } fields;
} bench_value_t;

static uint32_t s_keys[BENCH_MAX_CAPACITY];
static mstore_slot_t s_slots[BENCH_MAX_CAPACITY];
static uint32_t s_gens[BENCH_MAX_CAPACITY];
static uint32_t s_values[BENCH_MAX_CAPACITY]; /* актуальное поле a по slot */

static uint32_t s_live_keys[BENCH_MAX_CAPACITY];
static mstore_slot_t s_live_slots[BENCH_MAX_CAPACITY];
static uint32_t s_live_gens[BENCH_MAX_CAPACITY];
static size_t s_live_count;

static bench_samples_t s_allocate;
static bench_samples_t s_find;
static bench_samples_t s_read;
static bench_samples_t s_update_unchanged;
static bench_samples_t s_update_changed;
static bench_samples_t s_free;
static bench_samples_t s_iter;
static bench_samples_t s_clear;
static bench_samples_t s_reopen;
static bench_samples_t s_append_plain;
static bench_samples_t s_append_checkpoint;

static int64_t now_us(void) {
    return esp_timer_get_time();
}

static const char *mode_name(bench_mode_t mode) {
    switch (mode) {
        case BENCH_MODE_RAM:
            return "RAM";
        case BENCH_MODE_FLASH:
            return "FLASH";
        default:
            return "RAM_FLASH";
    }
}

static mstore_backing_t mode_backing(bench_mode_t mode) {
    switch (mode) {
        case BENCH_MODE_RAM:
            return MSTORE_BACKING_RAM;
        case BENCH_MODE_FLASH:
            return MSTORE_BACKING_FLASH;
        default:
            return MSTORE_BACKING_RAM | MSTORE_BACKING_FLASH;
    }
}

/* Ключ включает capacity: регион выделяется под конкретную геометрию и не меняется. */
static const char *mode_persist_key(bench_mode_t mode, size_t capacity) {
    static char key[40];
    if (mode == BENCH_MODE_RAM) {
        return NULL;
    }
    const char *base = (mode == BENCH_MODE_FLASH) ? "flash" : "ram_flash";
    snprintf(key, sizeof(key), "bench_%s_%u", base, (unsigned)capacity);
    return key;
}

static bool bench_open(mstore_table_t *table, bench_mode_t mode, size_t capacity) {
    mstore_table_schema_t schema;
    memset(&schema, 0, sizeof(schema));
    schema.capacity = capacity;
    schema.key_size = sizeof(uint32_t);
    schema.payload_size = BENCH_PAYLOAD_SIZE;
    schema.payload_equals = NULL;
    schema.backing = mode_backing(mode);
    schema.persist_key = mode_persist_key(mode, capacity);

    mstore_err_t err = mstore_table_init(table, &schema);
    if (err != MSTORE_OK) {
        printf("SKIP mode=%s capacity=%u err=%d\n", mode_name(mode), (unsigned)capacity, (int)err);
        return false;
    }
    return true;
}

static void reset_samples(void) {
    bench_samples_reset(&s_allocate);
    bench_samples_reset(&s_find);
    bench_samples_reset(&s_read);
    bench_samples_reset(&s_update_unchanged);
    bench_samples_reset(&s_update_changed);
    bench_samples_reset(&s_free);
    bench_samples_reset(&s_iter);
    bench_samples_reset(&s_clear);
    bench_samples_reset(&s_reopen);
    bench_samples_reset(&s_append_plain);
    bench_samples_reset(&s_append_checkpoint);
}

static void report_samples(bench_mode_t mode, size_t capacity, const char *op, bench_samples_t *samples) {
    bench_stats_t stats;
    if (samples->seen == 0) {
        return;
    }
    bench_samples_summarize(samples, &stats);
    bench_print_csv(mode_name(mode), capacity, BENCH_PAYLOAD_SIZE, op, &stats);
}

void bench_heap_snapshot(bench_heap_t *out) {
    out->free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    out->largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    out->minimum_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

static void report_memory(bench_mode_t mode, size_t capacity, const bench_heap_t *before_init,
                          const bench_heap_t *after_init, const bench_heap_t *after_fill,
                          const bench_heap_t *after_deinit) {
    printf("MEMORY mode=%s capacity=%u heap_before_init=%u heap_after_init=%u cost_init=%lld "
           "heap_after_fill=%u cost_fill=%lld min_free=%u largest_block=%u heap_after_deinit=%u "
           "leak_delta=%lld\n",
           mode_name(mode), (unsigned)capacity, (unsigned)before_init->free_bytes,
           (unsigned)after_init->free_bytes,
           (long long)before_init->free_bytes - (long long)after_init->free_bytes,
           (unsigned)after_fill->free_bytes,
           (long long)after_init->free_bytes - (long long)after_fill->free_bytes,
           (unsigned)after_fill->minimum_free, (unsigned)after_fill->largest_block,
           (unsigned)after_deinit->free_bytes,
           (long long)after_deinit->free_bytes - (long long)before_init->free_bytes);
}

static void report_flash(bench_mode_t mode, size_t capacity) {
#ifdef MSTORE_BENCH_COUNTERS
    if (mode == BENCH_MODE_RAM) {
        return;
    }
    mstore_bench_counters_t counters;
    mstore_bench_counters_read(&counters);
    printf("FLASH mode=%s capacity=%u records_appended=%llu compactions=%llu program_calls=%llu "
           "program_bytes=%llu erase_calls=%llu erase_bytes=%llu\n",
           mode_name(mode), (unsigned)capacity, (unsigned long long)counters.records_appended,
           (unsigned long long)counters.compactions, (unsigned long long)counters.program_calls,
           (unsigned long long)counters.program_bytes, (unsigned long long)counters.erase_calls,
           (unsigned long long)counters.erase_bytes);
#else
    (void)mode;
    (void)capacity;
#endif
}

static bench_value_t make_value(uint32_t a) {
    bench_value_t value;
    memset(&value, 0, sizeof(value));
    value.fields.a = a;
    value.fields.b = 1;
    return value;
}

static bool count_iter_cb(mstore_slot_t slot, const mstore_meta_t *meta, const void *key,
                          const void *payload, void *ctx) {
    (void)slot;
    (void)meta;
    (void)key;
    (void)payload;
    size_t *seen = (size_t *)ctx;
    (*seen)++;
    return true;
}

/* Заполнение без замеров: используется как подготовка сценария. */
static bool bench_fill(mstore_table_t *table, size_t capacity, uint32_t first_key) {
    for (size_t i = 0; i < capacity; i++) {
        s_keys[i] = first_key + (uint32_t)i;
        bench_value_t value = make_value((uint32_t)i);
        if (mstore_table_slot_allocate(table, &s_keys[i], &value, &s_slots[i], &s_gens[i]) !=
            MSTORE_OK) {
            printf("FAIL fill err\n");
            return false;
        }
        s_values[s_slots[i]] = value.fields.a;
    }
    return true;
}

static void bench_reset_live(size_t capacity) {
    s_live_count = capacity;
    for (size_t i = 0; i < capacity; i++) {
        s_live_keys[i] = s_keys[i];
        s_live_slots[i] = s_slots[i];
        s_live_gens[i] = s_gens[i];
    }
}

static void bench_live_drop(size_t index) {
    s_live_keys[index] = s_live_keys[s_live_count - 1];
    s_live_slots[index] = s_live_slots[s_live_count - 1];
    s_live_gens[index] = s_live_gens[s_live_count - 1];
    s_live_count--;
}

void bench_run_fill_read(bench_mode_t mode, size_t capacity, size_t ops) {
    if (capacity > BENCH_MAX_CAPACITY) {
        return;
    }
    bench_heap_t before_init;
    bench_heap_t after_init;
    bench_heap_t after_fill;
    bench_heap_t after_deinit;
    bench_heap_snapshot(&before_init);

    mstore_table_t table = {0};
    if (!bench_open(&table, mode, capacity)) {
        return;
    }
    bench_heap_snapshot(&after_init);

    reset_samples();
#ifdef MSTORE_BENCH_COUNTERS
    mstore_bench_counters_reset();
#endif
    mstore_table_clear(&table);

    for (size_t i = 0; i < capacity; i++) {
        s_keys[i] = (uint32_t)(i + 1);
        bench_value_t value = make_value((uint32_t)i);
        int64_t t0 = now_us();
        mstore_err_t err =
            mstore_table_slot_allocate(&table, &s_keys[i], &value, &s_slots[i], &s_gens[i]);
        int64_t t1 = now_us();
        if (err != MSTORE_OK) {
            printf("FAIL allocate err=%d\n", (int)err);
            return;
        }
        bench_samples_add(&s_allocate, (uint32_t)(t1 - t0));
        s_values[s_slots[i]] = value.fields.a;
    }
    bench_heap_snapshot(&after_fill);

    /* reopen: recovery на заполненной таблице */
    mstore_table_deinit(&table);
    memset(&table, 0, sizeof(table));
    int64_t t0 = now_us();
    if (!bench_open(&table, mode, capacity)) {
        return;
    }
    bench_samples_add(&s_reopen, (uint32_t)(now_us() - t0));

    for (size_t n = 0; n < ops; n++) {
        uint32_t key = s_keys[bench_rand_below((uint32_t)capacity)];
        mstore_slot_t found;
        t0 = now_us();
        mstore_table_slot_find(&table, &key, &found);
        bench_samples_add(&s_find, (uint32_t)(now_us() - t0));
    }

    for (size_t n = 0; n < ops; n++) {
        size_t index = bench_rand_below((uint32_t)capacity);
        mstore_meta_t meta;
        uint32_t read_key;
        bench_value_t read_value;
        t0 = now_us();
        mstore_table_slot_read(&table, s_slots[index], &meta, &read_key, &read_value);
        bench_samples_add(&s_read, (uint32_t)(now_us() - t0));
    }

    size_t update_ops = ops / 10 == 0 ? 1 : ops / 10;
    for (size_t n = 0; n < update_ops; n++) {
        size_t index = bench_rand_below((uint32_t)capacity);
        mstore_slot_t slot = s_slots[index];
        bench_value_t same = make_value(s_values[slot]);
        bool changed = true;
        t0 = now_us();
        mstore_table_slot_update(&table, slot, s_gens[index], &same, &changed);
        bench_samples_add(&s_update_unchanged, (uint32_t)(now_us() - t0));
    }

    for (size_t n = 0; n < update_ops; n++) {
        size_t index = bench_rand_below((uint32_t)capacity);
        mstore_slot_t slot = s_slots[index];
        bench_value_t next = make_value(s_values[slot] + 1);
        bool changed = false;
        t0 = now_us();
        mstore_table_slot_update(&table, slot, s_gens[index], &next, &changed);
        bench_samples_add(&s_update_changed, (uint32_t)(now_us() - t0));
        s_values[slot] = next.fields.a;
    }

    for (size_t n = 0; n < BENCH_ITER_ROUNDS; n++) {
        size_t seen = 0;
        t0 = now_us();
        mstore_table_iter(&table, count_iter_cb, &seen);
        bench_samples_add(&s_iter, (uint32_t)(now_us() - t0));
    }

    t0 = now_us();
    mstore_table_clear(&table);
    bench_samples_add(&s_clear, (uint32_t)(now_us() - t0));

    if (!bench_fill(&table, capacity, 1)) {
        return;
    }
    for (size_t i = 0; i < capacity; i++) {
        t0 = now_us();
        mstore_table_slot_free(&table, s_slots[i], s_gens[i]);
        bench_samples_add(&s_free, (uint32_t)(now_us() - t0));
    }

    mstore_table_deinit(&table);
    bench_heap_snapshot(&after_deinit);

    report_samples(mode, capacity, "allocate", &s_allocate);
    report_samples(mode, capacity, "find", &s_find);
    report_samples(mode, capacity, "read", &s_read);
    report_samples(mode, capacity, "update_unchanged", &s_update_unchanged);
    report_samples(mode, capacity, "update_changed", &s_update_changed);
    report_samples(mode, capacity, "iter", &s_iter);
    report_samples(mode, capacity, "clear", &s_clear);
    report_samples(mode, capacity, "free", &s_free);
    report_samples(mode, capacity, "reopen", &s_reopen);
    report_memory(mode, capacity, &before_init, &after_init, &after_fill, &after_deinit);
    report_flash(mode, capacity);
}

static volatile bool s_load_running;

static void bench_load_task(void *arg) {
    (void)arg;
    volatile uint64_t sink = 0;
    while (s_load_running) {
        sink = sink * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    vTaskDelete(NULL);
}

static void load_start(void) {
    s_load_running = true;
    xTaskCreatePinnedToCore(bench_load_task, "bench_load", 2048, NULL, BENCH_TASK_PRIORITY, NULL, 0);
}

static void load_stop(void) {
    s_load_running = false;
}

/* Одна смешанная итерация: 40% changed update, 30% read, 15% find, 10% free+allocate,
 * 5% unchanged update. Возвращает false при ошибке storage. */
static bool mixed_step(mstore_table_t *table, uint32_t *next_key) {
    uint32_t roll = bench_rand_below(1000);
    if (s_live_count == 0) {
        return true;
    }
    size_t index = bench_rand_below((uint32_t)s_live_count);
    mstore_slot_t slot = s_live_slots[index];
    uint32_t generation = s_live_gens[index];
    int64_t t0;
    mstore_err_t err;

    if (roll < 400) {
        bench_value_t value = make_value(s_values[slot] + 1);
        bool changed = false;
        t0 = now_us();
        err = mstore_table_slot_update(table, slot, generation, &value, &changed);
        bench_samples_add(&s_update_changed, (uint32_t)(now_us() - t0));
        if (err == MSTORE_OK) {
            s_values[slot] = value.fields.a;
        }
        return err == MSTORE_OK;
    }
    if (roll < 700) {
        mstore_meta_t meta;
        uint32_t read_key;
        bench_value_t read_value;
        t0 = now_us();
        err = mstore_table_slot_read(table, slot, &meta, &read_key, &read_value);
        bench_samples_add(&s_read, (uint32_t)(now_us() - t0));
        return err == MSTORE_OK;
    }
    if (roll < 850) {
        mstore_slot_t found;
        t0 = now_us();
        err = mstore_table_slot_find(table, &s_live_keys[index], &found);
        bench_samples_add(&s_find, (uint32_t)(now_us() - t0));
        return err == MSTORE_OK;
    }
    if (roll < 950) {
        t0 = now_us();
        err = mstore_table_slot_free(table, slot, generation);
        bench_samples_add(&s_free, (uint32_t)(now_us() - t0));
        if (err != MSTORE_OK) {
            return false;
        }
        bench_live_drop(index);
        uint32_t key = (*next_key)++;
        bench_value_t value = make_value(key);
        mstore_slot_t fresh_slot;
        uint32_t fresh_generation;
        t0 = now_us();
        err = mstore_table_slot_allocate(table, &key, &value, &fresh_slot, &fresh_generation);
        bench_samples_add(&s_allocate, (uint32_t)(now_us() - t0));
        if (err != MSTORE_OK) {
            return false;
        }
        s_values[fresh_slot] = value.fields.a;
        s_live_keys[s_live_count] = key;
        s_live_slots[s_live_count] = fresh_slot;
        s_live_gens[s_live_count] = fresh_generation;
        s_live_count++;
        return true;
    }
    bench_value_t same = make_value(s_values[slot]);
    bool changed = true;
    t0 = now_us();
    err = mstore_table_slot_update(table, slot, generation, &same, &changed);
    bench_samples_add(&s_update_unchanged, (uint32_t)(now_us() - t0));
    return err == MSTORE_OK;
}

static bool live_count_matches(mstore_table_t *table) {
    size_t count = 0;
    if (mstore_table_count(table, &count) != MSTORE_OK) {
        return false;
    }
    size_t seen = 0;
    if (mstore_table_iter(table, count_iter_cb, &seen) != MSTORE_OK) {
        return false;
    }
    return count == s_live_count && seen == s_live_count;
}

void bench_run_write_stress(bench_mode_t mode, size_t capacity, size_t ops, bool loaded) {
    if (capacity > BENCH_MAX_CAPACITY) {
        return;
    }
    mstore_table_t table = {0};
    if (!bench_open(&table, mode, capacity)) {
        return;
    }
    mstore_table_clear(&table);
    if (!bench_fill(&table, capacity, 1)) {
        mstore_table_deinit(&table);
        return;
    }
    bench_reset_live(capacity);

    reset_samples();
#ifdef MSTORE_BENCH_COUNTERS
    mstore_bench_counters_reset();
#endif
    uint32_t next_key = (uint32_t)capacity + 1;
    if (loaded) {
        load_start();
    }
    int64_t t0 = now_us();
    for (size_t n = 0; n < ops; n++) {
        if (!mixed_step(&table, &next_key)) {
            printf("FAIL stress step\n");
            break;
        }
    }
    uint32_t wall_us = (uint32_t)(now_us() - t0);
    if (loaded) {
        load_stop();
    }

    bool consistent = live_count_matches(&table);
    bench_heap_t heap;
    bench_heap_snapshot(&heap);
    mstore_table_deinit(&table);

    report_samples(mode, capacity, "stress_update_changed", &s_update_changed);
    report_samples(mode, capacity, "stress_read", &s_read);
    report_samples(mode, capacity, "stress_find", &s_find);
    report_samples(mode, capacity, "stress_free", &s_free);
    report_samples(mode, capacity, "stress_allocate", &s_allocate);
    report_samples(mode, capacity, "stress_update_unchanged", &s_update_unchanged);
    printf("STRESS mode=%s capacity=%u loaded=%d ops=%u wall_us=%u sustained_ops_sec=%.0f consistent=%d "
           "min_free=%u largest_block=%u\n",
           mode_name(mode), (unsigned)capacity, loaded ? 1 : 0, (unsigned)ops, (unsigned)wall_us,
           wall_us == 0 ? 0.0 : (double)ops * 1000000.0 / (double)wall_us, consistent ? 1 : 0,
           (unsigned)heap.minimum_free, (unsigned)heap.largest_block);
    report_flash(mode, capacity);
}

void bench_run_checkpoint(bench_mode_t mode, size_t capacity, size_t updates) {
    if (capacity > BENCH_MAX_CAPACITY) {
        return;
    }
    mstore_table_t table = {0};
    if (!bench_open(&table, mode, capacity)) {
        return;
    }
    mstore_table_clear(&table);
    if (!bench_fill(&table, capacity, 1)) {
        mstore_table_deinit(&table);
        return;
    }

    reset_samples();
#ifdef MSTORE_BENCH_COUNTERS
    mstore_bench_counters_reset();
#endif

    for (size_t n = 0; n < updates; n++) {
        size_t index = n % capacity;
        mstore_slot_t slot = s_slots[index];
        bench_value_t value = make_value(s_values[slot] + 1);
#ifdef MSTORE_BENCH_COUNTERS
        mstore_bench_counters_t before;
        mstore_bench_counters_t after;
        mstore_bench_counters_read(&before);
#endif
        bool changed = false;
        int64_t t0 = now_us();
        mstore_err_t err = mstore_table_slot_update(&table, slot, s_gens[index], &value, &changed);
        uint32_t elapsed = (uint32_t)(now_us() - t0);
#ifdef MSTORE_BENCH_COUNTERS
        mstore_bench_counters_read(&after);
#endif
        if (err != MSTORE_OK) {
            printf("FAIL checkpoint update err=%d\n", (int)err);
            break;
        }
        s_values[slot] = value.fields.a;
#ifdef MSTORE_BENCH_COUNTERS
        if (after.compactions > before.compactions) {
            bench_samples_add(&s_append_checkpoint, elapsed);
        } else {
            bench_samples_add(&s_append_plain, elapsed);
        }
#else
        bench_samples_add(&s_append_plain, elapsed);
#endif
    }

    mstore_table_deinit(&table);
    report_samples(mode, capacity, "update_plain", &s_append_plain);
    report_samples(mode, capacity, "update_checkpoint", &s_append_checkpoint);
    report_flash(mode, capacity);
}

void bench_run_soak(bench_mode_t mode, size_t capacity, uint32_t seconds) {
    if (seconds == 0 || capacity > BENCH_MAX_CAPACITY) {
        return;
    }
    mstore_table_t table = {0};
    if (!bench_open(&table, mode, capacity)) {
        return;
    }
    mstore_table_clear(&table);
    size_t filled = capacity / 2 == 0 ? 1 : capacity / 2;
    if (!bench_fill(&table, filled, 1)) {
        mstore_table_deinit(&table);
        return;
    }
    bench_reset_live(filled);

    uint32_t next_key = (uint32_t)filled + 1;
    size_t total_ops = 0;
    size_t reopens = 0;
    bool consistent = true;
    bool heap_ok = true;
    int64_t deadline = now_us() + (int64_t)seconds * 1000000;
    int64_t last_reopen = now_us();

    while (now_us() < deadline) {
        for (size_t n = 0; n < 2000 && now_us() < deadline; n++) {
            if (!mixed_step(&table, &next_key)) {
                consistent = false;
                break;
            }
            total_ops++;
        }
        vTaskDelay(1);
        if (now_us() - last_reopen > 60000000) {
            last_reopen = now_us();
            if (!live_count_matches(&table)) {
                consistent = false;
            }
            if (!heap_caps_check_integrity_all(false)) {
                heap_ok = false;
            }
            mstore_table_deinit(&table);
            memset(&table, 0, sizeof(table));
            if (!bench_open(&table, mode, capacity)) {
                return;
            }
            reopens++;
            bench_heap_t heap;
            bench_heap_snapshot(&heap);
            printf("SOAK mode=%s capacity=%u ops=%u reopens=%u free=%u min_free=%u largest=%u\n",
                   mode_name(mode), (unsigned)capacity, (unsigned)total_ops, (unsigned)reopens,
                   (unsigned)heap.free_bytes, (unsigned)heap.minimum_free,
                   (unsigned)heap.largest_block);
        }
    }

    if (!live_count_matches(&table)) {
        consistent = false;
    }
    if (!heap_caps_check_integrity_all(false)) {
        heap_ok = false;
    }
    bench_heap_t heap;
    bench_heap_snapshot(&heap);
    mstore_table_deinit(&table);

    printf("SOAK_SUMMARY mode=%s capacity=%u seconds=%u ops=%u reopens=%u consistent=%d heap_ok=%d "
           "min_free=%u largest_block=%u\n",
           mode_name(mode), (unsigned)capacity, (unsigned)seconds, (unsigned)total_ops,
           (unsigned)reopens, consistent ? 1 : 0, heap_ok ? 1 : 0, (unsigned)heap.minimum_free,
           (unsigned)heap.largest_block);
    report_flash(mode, capacity);
}
