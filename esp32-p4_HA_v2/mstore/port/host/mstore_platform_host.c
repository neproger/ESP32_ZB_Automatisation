#include "mstore_platform.h"

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

void *mstore_platform_alloc(size_t size) {
    return malloc(size);
}

void mstore_platform_free(void *ptr) {
    free(ptr);
}

#ifdef _WIN32

void *mstore_platform_lock_create(void) {
    CRITICAL_SECTION *lock = malloc(sizeof(*lock));
    if (lock == NULL) {
        return NULL;
    }
    InitializeCriticalSection(lock);
    return lock;
}

void mstore_platform_lock_destroy(void *lock) {
    DeleteCriticalSection((CRITICAL_SECTION *)lock);
    free(lock);
}

void mstore_platform_lock_acquire(void *lock) {
    EnterCriticalSection((CRITICAL_SECTION *)lock);
}

void mstore_platform_lock_release(void *lock) {
    LeaveCriticalSection((CRITICAL_SECTION *)lock);
}

#else

void *mstore_platform_lock_create(void) {
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

void mstore_platform_lock_destroy(void *lock) {
    pthread_mutex_destroy((pthread_mutex_t *)lock);
    free(lock);
}

void mstore_platform_lock_acquire(void *lock) {
    pthread_mutex_lock((pthread_mutex_t *)lock);
}

void mstore_platform_lock_release(void *lock) {
    pthread_mutex_unlock((pthread_mutex_t *)lock);
}

#endif

static const mstore_flash_device_t *s_flash_device;

const mstore_flash_device_t *mstore_platform_flash_device(void) {
    return s_flash_device;
}

void mstore_platform_flash_set_device(const mstore_flash_device_t *device) {
    s_flash_device = device;
}
