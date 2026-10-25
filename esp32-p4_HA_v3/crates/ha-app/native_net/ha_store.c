/* Partition access for MicroDB. Lookup by label, never by offset. */
#include <stddef.h>
#include <stdint.h>

#include "esp_partition.h"

static const esp_partition_t *partition_by_label(const char *label)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
}

static uint32_t partition_size(const char *label)
{
    const esp_partition_t *partition = partition_by_label(label);
    return partition != NULL ? (uint32_t)partition->size : 0;
}

static int partition_read(const char *label, uint32_t offset, void *buffer, size_t length)
{
    const esp_partition_t *partition = partition_by_label(label);
    if (partition == NULL || (uint64_t)offset + length > partition->size) {
        return -1;
    }
    return esp_partition_read(partition, offset, buffer, length) == ESP_OK ? 0 : -1;
}

static int partition_write(const char *label, uint32_t offset, const void *buffer, size_t length)
{
    const esp_partition_t *partition = partition_by_label(label);
    if (partition == NULL || (uint64_t)offset + length > partition->size) {
        return -1;
    }
    return esp_partition_write(partition, offset, buffer, length) == ESP_OK ? 0 : -1;
}

static int partition_erase(const char *label, uint32_t offset, size_t length)
{
    const esp_partition_t *partition = partition_by_label(label);
    if (partition == NULL || (uint64_t)offset + length > partition->size) {
        return -1;
    }
    return esp_partition_erase_range(partition, offset, length) == ESP_OK ? 0 : -1;
}

uint32_t ha_mstore_size(void) { return partition_size("mstore"); }
int ha_mstore_read(uint32_t offset, void *buffer, size_t length)
{
    return partition_read("mstore", offset, buffer, length);
}
int ha_mstore_write(uint32_t offset, const void *buffer, size_t length)
{
    return partition_write("mstore", offset, buffer, length);
}
int ha_mstore_erase(uint32_t offset, size_t length)
{
    return partition_erase("mstore", offset, length);
}

uint32_t ha_domain_size(void) { return partition_size("domain"); }
int ha_domain_read(uint32_t offset, void *buffer, size_t length)
{
    return partition_read("domain", offset, buffer, length);
}
int ha_domain_write(uint32_t offset, const void *buffer, size_t length)
{
    return partition_write("domain", offset, buffer, length);
}
int ha_domain_erase(uint32_t offset, size_t length)
{
    return partition_erase("domain", offset, length);
}
