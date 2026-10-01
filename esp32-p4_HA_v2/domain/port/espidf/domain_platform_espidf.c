#include "domain_platform.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

uint64_t domain_platform_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

void *domain_platform_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void domain_platform_free(void *ptr)
{
    heap_caps_free(ptr);
}

void *domain_platform_lock_create(void)
{
    return xSemaphoreCreateMutex();
}

void domain_platform_lock_destroy(void *lock)
{
    vSemaphoreDelete((SemaphoreHandle_t)lock);
}

void domain_platform_lock_acquire(void *lock)
{
    xSemaphoreTake((SemaphoreHandle_t)lock, portMAX_DELAY);
}

void domain_platform_lock_release(void *lock)
{
    xSemaphoreGive((SemaphoreHandle_t)lock);
}
