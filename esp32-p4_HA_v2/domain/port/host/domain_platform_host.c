#include "domain_platform.h"

#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef _WIN32

uint64_t domain_platform_now_ms(void)
{
    return GetTickCount64();
}

#else

uint64_t domain_platform_now_ms(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

#endif

void *domain_platform_alloc(size_t size)
{
    return malloc(size);
}

void domain_platform_free(void *ptr)
{
    free(ptr);
}

#ifdef _WIN32

void *domain_platform_lock_create(void)
{
    CRITICAL_SECTION *lock = malloc(sizeof(*lock));
    if (lock == NULL) {
        return NULL;
    }
    InitializeCriticalSection(lock);
    return lock;
}

void domain_platform_lock_destroy(void *lock)
{
    if (lock == NULL) {
        return;
    }
    DeleteCriticalSection((CRITICAL_SECTION *)lock);
    free(lock);
}

void domain_platform_lock_acquire(void *lock)
{
    EnterCriticalSection((CRITICAL_SECTION *)lock);
}

void domain_platform_lock_release(void *lock)
{
    LeaveCriticalSection((CRITICAL_SECTION *)lock);
}

typedef struct {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cond;
    bool raised;
} host_signal_t;

void *domain_platform_signal_create(void)
{
    host_signal_t *signal = malloc(sizeof(*signal));
    if (signal == NULL) {
        return NULL;
    }
    InitializeCriticalSection(&signal->lock);
    InitializeConditionVariable(&signal->cond);
    signal->raised = false;
    return signal;
}

void domain_platform_signal_destroy(void *signal)
{
    if (signal == NULL) {
        return;
    }
    host_signal_t *host = (host_signal_t *)signal;
    DeleteCriticalSection(&host->lock);
    free(host);
}

void domain_platform_signal_raise(void *signal)
{
    host_signal_t *host = (host_signal_t *)signal;
    EnterCriticalSection(&host->lock);
    host->raised = true;
    LeaveCriticalSection(&host->lock);
    WakeAllConditionVariable(&host->cond);
}

bool domain_platform_signal_wait(void *signal, uint32_t timeout_ms)
{
    host_signal_t *host = (host_signal_t *)signal;
    EnterCriticalSection(&host->lock);
    while (!host->raised) {
        if (!SleepConditionVariableCS(&host->cond, &host->lock, timeout_ms)) {
            LeaveCriticalSection(&host->lock);
            return false;
        }
    }
    host->raised = false;
    LeaveCriticalSection(&host->lock);
    return true;
}

#else

void *domain_platform_lock_create(void)

{
    pthread_mutex_t *lock = malloc(sizeof(*lock));
    if (lock == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(lock, NULL) != 0) {
        free(lock);
        return NULL;
    }
    return lock;
}

void domain_platform_lock_destroy(void *lock)
{
    if (lock == NULL) {
        return;
    }
    pthread_mutex_destroy((pthread_mutex_t *)lock);
    free(lock);
}

void domain_platform_lock_acquire(void *lock)
{
    pthread_mutex_lock((pthread_mutex_t *)lock);
}

void domain_platform_lock_release(void *lock)
{
    pthread_mutex_unlock((pthread_mutex_t *)lock);
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool raised;
} host_signal_t;

void *domain_platform_signal_create(void)
{
    host_signal_t *signal = malloc(sizeof(*signal));
    if (signal == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&signal->lock, NULL) != 0) {
        free(signal);
        return NULL;
    }
    pthread_cond_init(&signal->cond, NULL);
    signal->raised = false;
    return signal;
}

void domain_platform_signal_destroy(void *signal)
{
    if (signal == NULL) {
        return;
    }
    host_signal_t *host = (host_signal_t *)signal;
    pthread_cond_destroy(&host->cond);
    pthread_mutex_destroy(&host->lock);
    free(host);
}

void domain_platform_signal_raise(void *signal)
{
    host_signal_t *host = (host_signal_t *)signal;
    pthread_mutex_lock(&host->lock);
    host->raised = true;
    pthread_mutex_unlock(&host->lock);
    pthread_cond_broadcast(&host->cond);
}

bool domain_platform_signal_wait(void *signal, uint32_t timeout_ms)
{
    host_signal_t *host = (host_signal_t *)signal;
    pthread_mutex_lock(&host->lock);
    while (!host->raised) {
        struct timespec deadline = {0};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)(timeout_ms / 1000u);
        deadline.tv_nsec += (long)((timeout_ms % 1000u) * 1000000u);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        if (pthread_cond_timedwait(&host->cond, &host->lock, &deadline) != 0) {
            pthread_mutex_unlock(&host->lock);
            return false;
        }
    }
    host->raised = false;
    pthread_mutex_unlock(&host->lock);
    return true;
}

#endif
