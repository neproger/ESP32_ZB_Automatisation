#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Минимальный platform layer для Domain.
 * Реализуется портом (port/host, port/espidf); не является публичным API.
 * Domain не reuse'ит mstore_platform: он приватный для mstore.
 */
void *domain_platform_alloc(size_t size);
void domain_platform_free(void *ptr);

void *domain_platform_lock_create(void);
void domain_platform_lock_destroy(void *lock);
void domain_platform_lock_acquire(void *lock);
void domain_platform_lock_release(void *lock);

#ifdef __cplusplus
}
#endif
