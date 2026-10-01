#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Монотонное время для меток фактов; на target — esp_timer. */
uint64_t domain_platform_now_ms(void);

void *domain_platform_lock_create(void);
void domain_platform_lock_destroy(void *lock);
void domain_platform_lock_acquire(void *lock);
void domain_platform_lock_release(void *lock);

/*
 * Сигнал для пробуждения Dispatcher'а: raise после появления факта, wait с таймаутом
 * в задаче Dispatcher'а. Polling не используется (DISPATCHER.md §5).
 */
void *domain_platform_signal_create(void);
void domain_platform_signal_destroy(void *signal);
void domain_platform_signal_raise(void *signal);
bool domain_platform_signal_wait(void *signal, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
