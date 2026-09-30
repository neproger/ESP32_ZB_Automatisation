#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BENCH_MODE_RAM,
    BENCH_MODE_FLASH,
    BENCH_MODE_RAM_FLASH,
} bench_mode_t;

typedef struct {
    size_t free_bytes;
    size_t largest_block;
    size_t minimum_free;
} bench_heap_t;

void bench_heap_snapshot(bench_heap_t *out);

void bench_run_fill_read(bench_mode_t mode, size_t capacity, size_t ops);
void bench_run_write_stress(bench_mode_t mode, size_t capacity, size_t ops, bool loaded);
void bench_run_checkpoint(bench_mode_t mode, size_t capacity, size_t updates);
void bench_run_soak(bench_mode_t mode, size_t capacity, uint32_t seconds);

#ifdef __cplusplus
}
#endif
