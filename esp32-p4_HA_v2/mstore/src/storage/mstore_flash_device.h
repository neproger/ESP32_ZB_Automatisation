#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal NOR device seam: raw region primitives.
 * Host реализует через nor_sim; ESP-IDF — через esp_partition.
 * Программирование/erase возвращают false при физической ошибке; семантику
 * (logical commit, recovery) задаёт storage backend, а не device.
 */

typedef struct mstore_flash_device mstore_flash_device_t;

typedef struct {
    bool (*read)(void *ctx, size_t offset, void *dst, size_t len);
    bool (*program)(void *ctx, size_t offset, const void *src, size_t len);
    bool (*erase)(void *ctx, size_t offset, size_t len);
} mstore_flash_device_ops_t;

struct mstore_flash_device {
    const mstore_flash_device_ops_t *ops;
    void *ctx;
    size_t size;       /* размер региона */
    size_t erase_size; /* размер erase-блока */
};

#ifdef __cplusplus
}
#endif
