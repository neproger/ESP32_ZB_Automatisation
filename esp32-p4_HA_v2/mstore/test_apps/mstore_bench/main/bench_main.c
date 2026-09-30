#include <stdio.h>

#include "bench_run.h"
#include "bench_stats.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define BENCH_TASK_PRIORITY 5
#define BENCH_TASK_STACK 8192

static const size_t CAPACITIES[] = {100, 500, 1000, 2000, 5000};
#define CAPACITY_COUNT (sizeof(CAPACITIES) / sizeof(CAPACITIES[0]))

static void run_mode(bench_mode_t mode) {
    for (size_t i = 0; i < CAPACITY_COUNT; i++) {
        bench_run_fill_read(mode, CAPACITIES[i], CONFIG_BENCH_FIND_READ_OPS);
    }
    bench_run_checkpoint(mode, CONFIG_BENCH_CHECKPOINT_CAPACITY, CONFIG_BENCH_CHECKPOINT_UPDATES);
    bench_run_write_stress(mode, 1000, CONFIG_BENCH_STRESS_OPS, false);
#if CONFIG_BENCH_LOADED_MODE
    bench_run_write_stress(mode, 1000, CONFIG_BENCH_STRESS_OPS / 2, true);
#endif
    bench_run_soak(mode, 1000, CONFIG_BENCH_SOAK_SECONDS);
}

static void print_partition_info(void) {
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "mstore");
    if (partition == NULL) {
        printf("PARTITION mstore not found\n");
        return;
    }
    printf("PARTITION label=mstore address=0x%x size=%u erase_size=%u\n", (unsigned)partition->address,
           (unsigned)partition->size, (unsigned)partition->erase_size);
}

static void bench_task(void *arg) {
    (void)arg;
    bench_rand_seed(0x1234u);
    print_partition_info();
    bench_print_csv_header();
    printf("CONFIG cpu_mhz=%u find_read_ops=%u stress_ops=%u checkpoint_capacity=%u "
           "checkpoint_updates=%u soak_seconds=%u loaded=%d\n",
           (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, (unsigned)CONFIG_BENCH_FIND_READ_OPS,
           (unsigned)CONFIG_BENCH_STRESS_OPS, (unsigned)CONFIG_BENCH_CHECKPOINT_CAPACITY,
           (unsigned)CONFIG_BENCH_CHECKPOINT_UPDATES, (unsigned)CONFIG_BENCH_SOAK_SECONDS,
           CONFIG_BENCH_LOADED_MODE ? 1 : 0);

#if CONFIG_BENCH_MODE_RAM || CONFIG_BENCH_MODE_ALL
    run_mode(BENCH_MODE_RAM);
#endif
#if CONFIG_BENCH_MODE_FLASH || CONFIG_BENCH_MODE_ALL
    run_mode(BENCH_MODE_FLASH);
#endif
#if CONFIG_BENCH_MODE_RAM_FLASH || CONFIG_BENCH_MODE_ALL
    run_mode(BENCH_MODE_RAM_FLASH);
#endif

    printf("BENCH DONE\n");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    xTaskCreatePinnedToCore(bench_task, "bench", BENCH_TASK_STACK, NULL, BENCH_TASK_PRIORITY, NULL,
                            0);
}
