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

void *domain_platform_signal_create(void)
{
    return xSemaphoreCreateBinary();
}

void domain_platform_signal_destroy(void *signal)
{
    vSemaphoreDelete((SemaphoreHandle_t)signal);
}

void domain_platform_signal_raise(void *signal)
{
    xSemaphoreGive((SemaphoreHandle_t)signal);
}

bool domain_platform_signal_wait(void *signal, uint32_t timeout_ms)
{
    return xSemaphoreTake((SemaphoreHandle_t)signal, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
