#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Замер времени операции без printf внутри цикла: накапливаем выборку, печатаем
 * после сценария. Хранится не вся выборка, а reservoir ёмкостью
 * BENCH_SAMPLE_CAPACITY — процентили по ней, min/max/total — по всем операциям.
 */

#define BENCH_SAMPLE_CAPACITY 1024u

typedef struct {
    uint32_t samples[BENCH_SAMPLE_CAPACITY];
    uint32_t seen; /* всего замеров */
    size_t stored; /* сколько хранится (<= capacity) */
    uint64_t total_us;
    uint32_t min_us;
    uint32_t max_us;
} bench_samples_t;

typedef struct {
    size_t ops;
    uint64_t total_us;
    double ops_per_sec;
    double avg_us;
    uint32_t min_us;
    uint32_t p50_us;
    uint32_t p95_us;
    uint32_t p99_us;
    uint32_t max_us;
} bench_stats_t;

void bench_samples_reset(bench_samples_t *samples);
void bench_samples_add(bench_samples_t *samples, uint32_t us);
void bench_samples_summarize(const bench_samples_t *samples, bench_stats_t *out);

/* Детерминированный PRNG: сценарии воспроизводимы от запуска к запуску. */
void bench_rand_seed(uint32_t seed);
uint32_t bench_rand(void);
uint32_t bench_rand_below(uint32_t limit);

void bench_print_csv_header(void);
void bench_print_csv(const char *mode, size_t capacity, size_t payload, const char *operation,
                     const bench_stats_t *stats);

#ifdef __cplusplus
}
#endif
