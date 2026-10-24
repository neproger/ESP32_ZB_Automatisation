/* mstore partition access for MicroDB. Lookup by label, never by offset. */
#include <stddef.h>
#include <stdint.h>

#include "esp_partition.h"

static const esp_partition_t *mstore_partition(void)
{
    static const esp_partition_t *partition;
    if (partition == NULL) {
        partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                             "mstore");
    }
    return partition;
}

uint32_t ha_mstore_size(void)
{
    const esp_partition_t *partition = mstore_partition();
    return partition != NULL ? (uint32_t)partition->size : 0;
}

int ha_mstore_read(uint32_t offset, void *buffer, size_t length)
{
    const esp_partition_t *partition = mstore_partition();
    if (partition == NULL || offset + length > partition->size) {
        return -1;
    }
    return esp_partition_read(partition, offset, buffer, length) == ESP_OK ? 0 : -1;
}

int ha_mstore_write(uint32_t offset, const void *buffer, size_t length)
{
    const esp_partition_t *partition = mstore_partition();
    if (partition == NULL || offset + length > partition->size) {
        return -1;
    }
    return esp_partition_write(partition, offset, buffer, length) == ESP_OK ? 0 : -1;
}

int ha_mstore_erase(uint32_t offset, size_t length)
{
    const esp_partition_t *partition = mstore_partition();
    if (partition == NULL || offset + length > partition->size) {
        return -1;
    }
    return esp_partition_erase_range(partition, offset, length) == ESP_OK ? 0 : -1;
}
