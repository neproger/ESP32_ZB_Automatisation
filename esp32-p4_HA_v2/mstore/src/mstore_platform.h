#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Минимальный platform layer для mstore core.
 * Реализуется портом (port/host, port/espidf); не является публичным API.
 */

void *mstore_platform_alloc(size_t size);
void mstore_platform_free(void *ptr);

void *mstore_platform_lock_create(void);
void mstore_platform_lock_destroy(void *lock);
void mstore_platform_lock_acquire(void *lock);
void mstore_platform_lock_release(void *lock);

/* NOR-регион для FLASH backing. Host — установленный тестом device;
 * ESP-IDF — partition (позже). NULL означает "flash не сконфигурирован". */
typedef struct mstore_flash_device mstore_flash_device_t;
const mstore_flash_device_t *mstore_platform_flash_device(void);
void mstore_platform_flash_set_device(const mstore_flash_device_t *device);

#ifdef __cplusplus
}
#endif
