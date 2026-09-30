#include "mstore_platform.h"

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "storage/mstore_flash_device.h"

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

static bool partition_read(void *ctx, size_t offset, void *dst, size_t len) {
    return esp_partition_read((const esp_partition_t *)ctx, offset, dst, len) == ESP_OK;
}

static bool partition_program(void *ctx, size_t offset, const void *src, size_t len) {
    return esp_partition_write((const esp_partition_t *)ctx, offset, src, len) == ESP_OK;
}

static bool partition_erase(void *ctx, size_t offset, size_t len) {
    return esp_partition_erase_range((const esp_partition_t *)ctx, offset, len) == ESP_OK;
}

static const mstore_flash_device_ops_t PARTITION_OPS = {
    .read = partition_read,
    .program = partition_program,
    .erase = partition_erase,
};

static const mstore_flash_device_t *s_flash_device;
static mstore_flash_device_t s_partition_device;

/* Нор-регион по умолчанию — data-partition с меткой CONFIG_MSTORE_FLASH_PARTITION_LABEL.
 * set_device() позволяет подменить (например, тестовый device). */
const mstore_flash_device_t *mstore_platform_flash_device(void) {
    if (s_flash_device != NULL) {
        return s_flash_device;
    }
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CONFIG_MSTORE_FLASH_PARTITION_LABEL);
    if (partition == NULL) {
        return NULL;
    }
    s_partition_device.ops = &PARTITION_OPS;
    s_partition_device.ctx = (void *)partition;
    s_partition_device.size = partition->size;
    s_partition_device.erase_size = partition->erase_size;
    s_flash_device = &s_partition_device;
    return s_flash_device;
}

void mstore_platform_flash_set_device(const mstore_flash_device_t *device) {
    s_flash_device = device;
}
