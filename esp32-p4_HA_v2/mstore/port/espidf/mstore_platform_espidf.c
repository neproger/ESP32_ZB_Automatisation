#include "mstore_platform.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void *mstore_platform_alloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void mstore_platform_free(void *ptr) {
    heap_caps_free(ptr);
}

void *mstore_platform_lock_create(void) {
    return xSemaphoreCreateMutex();
}

void mstore_platform_lock_destroy(void *lock) {
    vSemaphoreDelete((SemaphoreHandle_t)lock);
}

void mstore_platform_lock_acquire(void *lock) {
    xSemaphoreTake((SemaphoreHandle_t)lock, portMAX_DELAY);
}

void mstore_platform_lock_release(void *lock) {
    xSemaphoreGive((SemaphoreHandle_t)lock);
}
