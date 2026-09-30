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

#ifdef __cplusplus
}
#endif
