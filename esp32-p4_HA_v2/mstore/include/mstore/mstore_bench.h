#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Счётчики FLASH backend для measurement (benchmark build).
 *
 * Это не часть Table API: Table Engine их не использует, семантика таблицы от них
 * не зависит. Определяются только в сборке с MSTORE_BENCH_COUNTERS (её задаёт
 * benchmark-приложение), поэтому обычная сборка не несёт лишнего кода и состояния.
 */

#ifdef MSTORE_BENCH_COUNTERS

typedef struct {
    uint64_t records_appended;
    uint64_t compactions; /* checkpoint и atomic clear: оба идут через compaction */
    uint64_t program_calls;
    uint64_t program_bytes;
    uint64_t erase_calls;
    uint64_t erase_bytes;
} mstore_bench_counters_t;

void mstore_bench_counters_reset(void);
void mstore_bench_counters_read(mstore_bench_counters_t *out);

#endif /* MSTORE_BENCH_COUNTERS */

#ifdef __cplusplus
}
#endif
