#include "domain_platform.h"

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
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

#endif
