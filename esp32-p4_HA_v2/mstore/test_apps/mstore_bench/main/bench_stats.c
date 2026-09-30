#include "bench_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_rng_state = 0x9E3779B9u;

void bench_rand_seed(uint32_t seed) {
    s_rng_state = seed == 0u ? 0x9E3779B9u : seed;
}

uint32_t bench_rand(void) {
    s_rng_state ^= s_rng_state << 13;
    s_rng_state ^= s_rng_state >> 17;
    s_rng_state ^= s_rng_state << 5;
    return s_rng_state;
}

uint32_t bench_rand_below(uint32_t limit) {
    return bench_rand() % limit;
}

void bench_samples_reset(bench_samples_t *samples) {
    samples->seen = 0;
    samples->stored = 0;
    samples->total_us = 0;
    samples->min_us = UINT32_MAX;
    samples->max_us = 0;
}

void bench_samples_add(bench_samples_t *samples, uint32_t us) {
    samples->seen++;
    samples->total_us += us;
    if (us < samples->min_us) {
        samples->min_us = us;
    }
    if (us > samples->max_us) {
        samples->max_us = us;
    }
    if (samples->stored < BENCH_SAMPLE_CAPACITY) {
        samples->samples[samples->stored++] = us;
        return;
    }
    /* Reservoir: каждый последующий замер попадает в выборку с равной вероятностью. */
    uint32_t slot = bench_rand() % samples->seen;
    if (slot < BENCH_SAMPLE_CAPACITY) {
        samples->samples[slot] = us;
    }
}

static uint32_t percentile(const uint32_t *sorted, size_t count, uint32_t percent) {
    if (count == 0) {
        return 0;
    }
    size_t index = (size_t)((count - 1) * percent / 100u);
    return sorted[index];
}

static int compare_u32(const void *lhs, const void *rhs) {
    uint32_t a = *(const uint32_t *)lhs;
    uint32_t b = *(const uint32_t *)rhs;
    return (a > b) - (a < b);
}

void bench_samples_summarize(const bench_samples_t *samples, bench_stats_t *out) {
    static uint32_t sorted[BENCH_SAMPLE_CAPACITY];

    out->ops = samples->seen;
    out->total_us = samples->total_us;
    out->min_us = samples->seen == 0 ? 0 : samples->min_us;
    out->max_us = samples->max_us;
    out->avg_us = samples->seen == 0 ? 0.0 : (double)samples->total_us / (double)samples->seen;
    out->ops_per_sec = samples->total_us == 0 ? 0.0
                                              : (double)samples->seen * 1000000.0 / (double)samples->total_us;

    if (samples->stored == 0) {
        out->p50_us = out->p95_us = out->p99_us = 0;
        return;
    }
    memcpy(sorted, samples->samples, samples->stored * sizeof(sorted[0]));
    qsort(sorted, samples->stored, sizeof(sorted[0]), compare_u32);
    out->p50_us = percentile(sorted, samples->stored, 50);
    out->p95_us = percentile(sorted, samples->stored, 95);
    out->p99_us = percentile(sorted, samples->stored, 99);
}

void bench_print_csv_header(void) {
    printf("MODE,capacity,payload,operation,ops,total_us,ops_sec,avg_us,p50,p95,p99,max\n");
}

void bench_print_csv(const char *mode, size_t capacity, size_t payload, const char *operation,
                     const bench_stats_t *stats) {
    printf("%s,%u,%u,%s,%u,%llu,%.0f,%.2f,%u,%u,%u,%u\n", mode, (unsigned)capacity, (unsigned)payload,
           operation, (unsigned)stats->ops, (unsigned long long)stats->total_us, stats->ops_per_sec,
           stats->avg_us, (unsigned)stats->p50_us, (unsigned)stats->p95_us, (unsigned)stats->p99_us,
           (unsigned)stats->max_us);
}
