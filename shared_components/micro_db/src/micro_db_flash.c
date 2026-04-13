#include "micro_db/micro_db_flash.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum {
    MICRO_DB_FLASH_MAX_TABLES = 16,
    MICRO_DB_FLASH_LAYOUT_VERSION = 2,
    MICRO_DB_FLASH_SECTOR_SIZE = 0x1000,
    MICRO_DB_FLASH_DIR_SECTOR = 0x1000,
    MICRO_DB_FLASH_ALIGN = 0x1000,
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_count;
    uint32_t next_free_offset;
    uint32_t reserved[3];
} micro_db_flash_dir_hdr_t;

typedef struct {
    uint8_t used;
    uint8_t reserved0[3];
    char persist_key[16];
    uint32_t region_offset;
    uint32_t region_size;
    uint32_t record_size;
    uint32_t capacity;
    uint32_t reserved1;
} micro_db_flash_dir_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    uint32_t record_size;
    uint32_t capacity;
    uint32_t live_count;
} micro_db_flash_table_hdr_t;

typedef struct {
    micro_db_flash_dir_hdr_t hdr;
    micro_db_flash_dir_entry_t entries[MICRO_DB_FLASH_MAX_TABLES];
} micro_db_flash_dir_page_t;

static const char *TAG = "micro_db_flash";
static const char *MICRO_DB_FLASH_PARTITION = "gw_data";
static const uint32_t MICRO_DB_FLASH_DIR_MAGIC = 0x4642444Du;   /* MDBF */
static const uint32_t MICRO_DB_FLASH_TABLE_MAGIC = 0x5442444Du; /* MDBT */

static const esp_partition_t *s_part = NULL;
static micro_db_flash_dir_page_t s_dir;
static uint8_t s_sector_buf[MICRO_DB_FLASH_SECTOR_SIZE];
static const uint8_t s_zero_buf[MICRO_DB_FLASH_SECTOR_SIZE] = {0};
static SemaphoreHandle_t s_flash_lock;
static bool s_ready = false;

static esp_err_t flash_lock(void)
{
    if (s_flash_lock == NULL) {
        s_flash_lock = xSemaphoreCreateRecursiveMutex();
        if (s_flash_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTakeRecursive(s_flash_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void flash_unlock(void)
{
    if (s_flash_lock != NULL) {
        (void)xSemaphoreGiveRecursive(s_flash_lock);
    }
}

static size_t align_up(size_t value, size_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

static size_t table_slot_used_size(const micro_db_table_t *table)
{
    return table->capacity * sizeof(uint8_t);
}

static size_t table_slot_crc_size(const micro_db_table_t *table)
{
    return table->capacity * sizeof(uint32_t);
}

static size_t table_records_size(const micro_db_table_t *table)
{
    return table->capacity * table->schema->record_size;
}

static size_t table_region_size(const micro_db_table_t *table)
{
    const size_t raw = sizeof(micro_db_flash_table_hdr_t) +
                       table_slot_used_size(table) +
                       table_slot_crc_size(table) +
                       table_records_size(table);
    return align_up(raw, MICRO_DB_FLASH_ALIGN);
}

static uint32_t table_slot_used_offset(const micro_db_flash_dir_entry_t *entry)
{
    return entry->region_offset + (uint32_t)sizeof(micro_db_flash_table_hdr_t);
}

static uint32_t table_slot_crc_offset(const micro_db_flash_dir_entry_t *entry)
{
    return table_slot_used_offset(entry) + entry->capacity;
}

static uint32_t table_records_offset(const micro_db_flash_dir_entry_t *entry)
{
    return table_slot_crc_offset(entry) + (entry->capacity * sizeof(uint32_t));
}

static uint32_t record_checksum32(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static esp_err_t patch_partition(uint32_t offset, const void *data, size_t len)
{
    if (!s_part || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((offset + len) > s_part->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t lock_err = flash_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    const uint8_t *src = (const uint8_t *)data;
    size_t remaining = len;
    uint32_t cur = offset;

    while (remaining > 0) {
        const uint32_t sector_base = cur & ~(MICRO_DB_FLASH_SECTOR_SIZE - 1u);
        const size_t sector_off = cur - sector_base;
        const size_t chunk = (remaining < (MICRO_DB_FLASH_SECTOR_SIZE - sector_off))
                                 ? remaining
                                 : (MICRO_DB_FLASH_SECTOR_SIZE - sector_off);

        esp_err_t err = esp_partition_read(s_part, sector_base, s_sector_buf, sizeof(s_sector_buf));
        if (err != ESP_OK) {
            flash_unlock();
            return err;
        }

        memcpy(s_sector_buf + sector_off, src, chunk);

        err = esp_partition_erase_range(s_part, sector_base, MICRO_DB_FLASH_SECTOR_SIZE);
        if (err != ESP_OK) {
            flash_unlock();
            return err;
        }

        err = esp_partition_write(s_part, sector_base, s_sector_buf, sizeof(s_sector_buf));
        if (err != ESP_OK) {
            flash_unlock();
            return err;
        }

        cur += (uint32_t)chunk;
        src += chunk;
        remaining -= chunk;
    }

    flash_unlock();
    return ESP_OK;
}

static esp_err_t write_zeros(uint32_t offset, size_t len)
{
    size_t remaining = len;
    uint32_t cur = offset;

    while (remaining > 0) {
        const size_t chunk = remaining < sizeof(s_zero_buf) ? remaining : sizeof(s_zero_buf);
        esp_err_t err = patch_partition(cur, s_zero_buf, chunk);
        if (err != ESP_OK) {
            return err;
        }
        cur += (uint32_t)chunk;
        remaining -= chunk;
    }

    return ESP_OK;
}

static esp_err_t persist_directory(void)
{
    return patch_partition(0, &s_dir, sizeof(s_dir));
}

static esp_err_t ensure_ready(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, MICRO_DB_FLASH_PARTITION);
    if (!s_part) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(&s_dir, 0, sizeof(s_dir));

    esp_err_t err = esp_partition_read(s_part, 0, &s_dir, sizeof(s_dir));
    if (err != ESP_OK) {
        return err;
    }

    if (s_dir.hdr.magic != MICRO_DB_FLASH_DIR_MAGIC ||
        s_dir.hdr.version != MICRO_DB_FLASH_LAYOUT_VERSION ||
        s_dir.hdr.next_free_offset < MICRO_DB_FLASH_DIR_SECTOR ||
        s_dir.hdr.next_free_offset > s_part->size) {
        memset(&s_dir, 0, sizeof(s_dir));
        s_dir.hdr.magic = MICRO_DB_FLASH_DIR_MAGIC;
        s_dir.hdr.version = MICRO_DB_FLASH_LAYOUT_VERSION;
        s_dir.hdr.entry_count = 0;
        s_dir.hdr.next_free_offset = MICRO_DB_FLASH_DIR_SECTOR;
    }

    s_ready = true;
    return ESP_OK;
}

static int find_entry_index(const char *persist_key)
{
    if (!persist_key || !persist_key[0]) {
        return -1;
    }

    for (size_t i = 0; i < MICRO_DB_FLASH_MAX_TABLES; ++i) {
        if (!s_dir.entries[i].used) {
            continue;
        }
        if (strncmp(s_dir.entries[i].persist_key, persist_key, sizeof(s_dir.entries[i].persist_key)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t drop_entry(int idx)
{
    if (idx < 0 || idx >= MICRO_DB_FLASH_MAX_TABLES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_dir.entries[idx].used) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(&s_dir.entries[idx], 0, sizeof(s_dir.entries[idx]));
    if (s_dir.hdr.entry_count > 0) {
        s_dir.hdr.entry_count--;
    }
    return persist_directory();
}

static bool entry_region_eraseable(const micro_db_flash_dir_entry_t *entry)
{
    if (entry == NULL || s_part == NULL) {
        return false;
    }
    if (entry->region_offset < MICRO_DB_FLASH_DIR_SECTOR) {
        return false;
    }
    if ((entry->region_offset % MICRO_DB_FLASH_SECTOR_SIZE) != 0u) {
        return false;
    }
    if ((entry->region_size % MICRO_DB_FLASH_SECTOR_SIZE) != 0u) {
        return false;
    }
    if ((entry->region_offset + entry->region_size) > s_part->size) {
        return false;
    }
    return true;
}

static esp_err_t compact_directory_locked(void)
{
    if (!s_part) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t next = MICRO_DB_FLASH_DIR_SECTOR;
    size_t moved = 0;

    for (size_t packed = 0; packed < MICRO_DB_FLASH_MAX_TABLES; ++packed) {
        int best = -1;
        uint32_t best_offset = UINT32_MAX;
        for (size_t i = 0; i < MICRO_DB_FLASH_MAX_TABLES; ++i) {
            const micro_db_flash_dir_entry_t *candidate = &s_dir.entries[i];
            if (!candidate->used || !entry_region_eraseable(candidate)) {
                continue;
            }
            if (candidate->region_offset < next) {
                continue;
            }
            if (candidate->region_offset < best_offset) {
                best_offset = candidate->region_offset;
                best = (int)i;
            }
        }
        if (best < 0) {
            break;
        }

        micro_db_flash_dir_entry_t *entry = &s_dir.entries[best];
        const uint32_t new_offset = (uint32_t)align_up(next, MICRO_DB_FLASH_ALIGN);
        next = new_offset + entry->region_size;

        if (entry->region_offset == new_offset) {
            continue;
        }

        uint8_t *region = (uint8_t *)malloc(entry->region_size);
        if (!region) {
            return ESP_ERR_NO_MEM;
        }

        esp_err_t err = esp_partition_read(s_part, entry->region_offset, region, entry->region_size);
        if (err != ESP_OK) {
            free(region);
            return err;
        }

        err = esp_partition_erase_range(s_part, new_offset, entry->region_size);
        if (err != ESP_OK) {
            free(region);
            return err;
        }

        err = esp_partition_write(s_part, new_offset, region, entry->region_size);
        free(region);
        if (err != ESP_OK) {
            return err;
        }

        ESP_LOGW(TAG,
                 "compacted table key=%s 0x%08x->0x%08x size=0x%08x",
                 entry->persist_key,
                 (unsigned)entry->region_offset,
                 (unsigned)new_offset,
                 (unsigned)entry->region_size);
        entry->region_offset = new_offset;
        moved++;
    }

    const uint32_t old_next = s_dir.hdr.next_free_offset;
    s_dir.hdr.next_free_offset = (uint32_t)align_up(next, MICRO_DB_FLASH_ALIGN);
    esp_err_t err = persist_directory();
    if (err == ESP_OK) {
        ESP_LOGW(TAG,
                 "compacted directory entries=%u moved=%u next_free 0x%08x->0x%08x part=0x%08x",
                 (unsigned)s_dir.hdr.entry_count,
                 (unsigned)moved,
                 (unsigned)old_next,
                 (unsigned)s_dir.hdr.next_free_offset,
                 (unsigned)s_part->size);
    }
    return err;
}

static esp_err_t allocate_entry(const micro_db_table_t *table, int *out_index)
{
    if (!table || !table->schema || !table->schema->persist_key || !out_index) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < MICRO_DB_FLASH_MAX_TABLES; ++i) {
        if (s_dir.entries[i].used) {
            continue;
        }

        const size_t region_size = table_region_size(table);
        uint32_t region_offset = (uint32_t)align_up(s_dir.hdr.next_free_offset, MICRO_DB_FLASH_ALIGN);
        if ((region_offset + region_size) > s_part->size) {
            esp_err_t compact_err = compact_directory_locked();
            if (compact_err == ESP_OK) {
                region_offset = (uint32_t)align_up(s_dir.hdr.next_free_offset, MICRO_DB_FLASH_ALIGN);
            } else {
                ESP_LOGW(TAG,
                         "directory compaction failed table=%s key=%s err=%s",
                         table->schema->name ? table->schema->name : "(unnamed)",
                         table->schema->persist_key ? table->schema->persist_key : "(none)",
                         esp_err_to_name(compact_err));
            }
        }
        if ((region_offset + region_size) > s_part->size) {
            ESP_LOGW(TAG,
                     "no flash space for table=%s key=%s region=0x%08x+0x%08x part=0x%08x entries=%u",
                     table->schema->name ? table->schema->name : "(unnamed)",
                     table->schema->persist_key ? table->schema->persist_key : "(none)",
                     (unsigned)region_offset,
                     (unsigned)region_size,
                     (unsigned)s_part->size,
                     (unsigned)s_dir.hdr.entry_count);
            return ESP_ERR_NO_MEM;
        }

        micro_db_flash_dir_entry_t *entry = &s_dir.entries[i];
        memset(entry, 0, sizeof(*entry));
        entry->used = 1;
        snprintf(entry->persist_key, sizeof(entry->persist_key), "%s", table->schema->persist_key);
        entry->region_offset = region_offset;
        entry->region_size = (uint32_t)region_size;
        entry->record_size = (uint32_t)table->schema->record_size;
        entry->capacity = (uint32_t)table->capacity;

        s_dir.hdr.entry_count++;
        s_dir.hdr.next_free_offset = region_offset + (uint32_t)region_size;

        esp_err_t err = persist_directory();
        if (err != ESP_OK) {
            memset(entry, 0, sizeof(*entry));
            s_dir.hdr.entry_count--;
            s_dir.hdr.next_free_offset = region_offset;
            return err;
        }

        err = esp_partition_erase_range(s_part, region_offset, region_size);
        if (err != ESP_OK) {
            return err;
        }

        err = write_zeros(table_slot_used_offset(entry), table->capacity * sizeof(uint8_t));
        if (err != ESP_OK) {
            return err;
        }

        err = write_zeros(table_slot_crc_offset(entry), table->capacity * sizeof(uint32_t));
        if (err != ESP_OK) {
            return err;
        }

        err = write_zeros(table_records_offset(entry),
                          (size_t)table->capacity * table->schema->record_size);
        if (err != ESP_OK) {
            return err;
        }

        const micro_db_flash_table_hdr_t hdr = {
            .magic = MICRO_DB_FLASH_TABLE_MAGIC,
            .version = MICRO_DB_FLASH_LAYOUT_VERSION,
            .reserved0 = 0,
            .record_size = (uint32_t)table->schema->record_size,
            .capacity = (uint32_t)table->capacity,
            .live_count = 0,
        };
        err = patch_partition(entry->region_offset, &hdr, sizeof(hdr));
        if (err != ESP_OK) {
            return err;
        }

        *out_index = (int)i;
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "no directory slot for table=%s key=%s max_tables=%u entries=%u",
             table->schema->name ? table->schema->name : "(unnamed)",
             table->schema->persist_key ? table->schema->persist_key : "(none)",
             (unsigned)MICRO_DB_FLASH_MAX_TABLES,
             (unsigned)s_dir.hdr.entry_count);
    return ESP_ERR_NO_MEM;
}

static esp_err_t get_or_create_entry(const micro_db_table_t *table,
                                     bool create,
                                     micro_db_flash_dir_entry_t **out_entry)
{
    if (!table || !table->schema || !table->schema->persist_key || !out_entry) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t lock_err = flash_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    esp_err_t err = ensure_ready();
    if (err != ESP_OK) {
        flash_unlock();
        return err;
    }

    int idx = find_entry_index(table->schema->persist_key);
    if (idx < 0 && create) {
        ESP_LOGW(TAG,
                 "flash table entry missing, allocating table=%s key=%s rec=%u cap=%u",
                 table->schema->name ? table->schema->name : "(unnamed)",
                 table->schema->persist_key ? table->schema->persist_key : "(none)",
                 (unsigned)table->schema->record_size,
                 (unsigned)table->capacity);
        err = allocate_entry(table, &idx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "flash table allocate failed table=%s key=%s err=%s",
                     table->schema->name ? table->schema->name : "(unnamed)",
                     table->schema->persist_key ? table->schema->persist_key : "(none)",
                     esp_err_to_name(err));
            flash_unlock();
            return err;
        }
    }
    if (idx < 0) {
        flash_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    micro_db_flash_dir_entry_t *entry = &s_dir.entries[idx];
    if (entry->record_size != table->schema->record_size || entry->capacity != table->capacity) {
        flash_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    *out_entry = entry;
    flash_unlock();
    return ESP_OK;
}

esp_err_t micro_db_flash_init(void)
{
    return ensure_ready();
}

esp_err_t micro_db_flash_write_meta(const micro_db_table_t *table)
{
    micro_db_flash_dir_entry_t *entry = NULL;
    esp_err_t err = get_or_create_entry(table, true, &entry);
    if (err != ESP_OK) {
        return err;
    }

    const micro_db_flash_table_hdr_t hdr = {
        .magic = MICRO_DB_FLASH_TABLE_MAGIC,
        .version = MICRO_DB_FLASH_LAYOUT_VERSION,
        .reserved0 = 0,
        .record_size = (uint32_t)table->schema->record_size,
        .capacity = (uint32_t)table->capacity,
        .live_count = (uint32_t)table->live_count,
    };
    return patch_partition(entry->region_offset, &hdr, sizeof(hdr));
}

esp_err_t micro_db_flash_write_slot_used(const micro_db_table_t *table, uint32_t slot)
{
    micro_db_flash_dir_entry_t *entry = NULL;
    esp_err_t err = get_or_create_entry(table, true, &entry);
    if (err != ESP_OK) {
        return err;
    }
    if (slot >= entry->capacity) {
        return ESP_ERR_INVALID_ARG;
    }

    return patch_partition(table_slot_used_offset(entry) + slot, &table->slot_used[slot], sizeof(uint8_t));
}

esp_err_t micro_db_flash_write_slot(const micro_db_table_t *table, uint32_t slot)
{
    micro_db_flash_dir_entry_t *entry = NULL;
    esp_err_t err = get_or_create_entry(table, true, &entry);
    if (err != ESP_OK) {
        return err;
    }
    if (slot >= entry->capacity) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *base = (const uint8_t *)table->records;
    const uint8_t *record = base + (slot * table->schema->record_size);
    err = patch_partition(table_records_offset(entry) + (slot * table->schema->record_size),
                          record,
                          table->schema->record_size);
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t checksum = record_checksum32(record, table->schema->record_size);
    return patch_partition(table_slot_crc_offset(entry) + (slot * sizeof(uint32_t)),
                           &checksum,
                           sizeof(checksum));
}

esp_err_t micro_db_flash_persist_table(const micro_db_table_t *table)
{
    micro_db_flash_dir_entry_t *entry = NULL;
    esp_err_t err = get_or_create_entry(table, true, &entry);
    if (err != ESP_OK) {
        return err;
    }

    err = micro_db_flash_write_meta(table);
    if (err != ESP_OK) {
        return err;
    }

    err = patch_partition(table_slot_used_offset(entry), table->slot_used, table_slot_used_size(table));
    if (err != ESP_OK) {
        return err;
    }

    uint32_t *crc_buf = (uint32_t *)calloc(table->capacity, sizeof(uint32_t));
    if (!crc_buf) {
        return ESP_ERR_NO_MEM;
    }

    const uint8_t *base = (const uint8_t *)table->records;
    for (size_t i = 0; i < table->capacity; ++i) {
        if (!table->slot_used[i]) {
            crc_buf[i] = 0u;
            continue;
        }
        const uint8_t *record = base + (i * table->schema->record_size);
        crc_buf[i] = record_checksum32(record, table->schema->record_size);
    }

    err = patch_partition(table_slot_crc_offset(entry), crc_buf, table_slot_crc_size(table));
    free(crc_buf);
    if (err != ESP_OK) {
        return err;
    }

    return patch_partition(table_records_offset(entry), table->records, table_records_size(table));
}

esp_err_t micro_db_flash_clear_table(const micro_db_table_t *table)
{
    if (!table || !table->schema || !table->schema->persist_key) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_ready();
    if (err != ESP_OK) {
        return err;
    }

    int idx = find_entry_index(table->schema->persist_key);
    micro_db_flash_dir_entry_t *entry = NULL;
    if (idx >= 0) {
        micro_db_flash_dir_entry_t *existing = &s_dir.entries[idx];
        const bool schema_matches =
            existing->record_size == table->schema->record_size &&
            existing->capacity == table->capacity;

        if (schema_matches && entry_region_eraseable(existing)) {
            err = esp_partition_erase_range(s_part, existing->region_offset, existing->region_size);
            if (err == ESP_OK) {
                entry = existing;
                goto write_fresh_image;
            }
        }

        err = drop_entry(idx);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = get_or_create_entry(table, true, &entry);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_partition_erase_range(s_part, entry->region_offset, entry->region_size);
    if (err != ESP_OK) {
        return err;
    }

write_fresh_image:
    micro_db_flash_table_hdr_t hdr = {
        .magic = MICRO_DB_FLASH_TABLE_MAGIC,
        .version = MICRO_DB_FLASH_LAYOUT_VERSION,
        .reserved0 = 0,
        .record_size = (uint32_t)table->schema->record_size,
        .capacity = (uint32_t)table->capacity,
        .live_count = 0,
    };
    err = patch_partition(entry->region_offset, &hdr, sizeof(hdr));
    if (err != ESP_OK) {
        return err;
    }

    err = write_zeros(table_slot_used_offset(entry), table->capacity * sizeof(uint8_t));
    if (err != ESP_OK) {
        return err;
    }

    err = write_zeros(table_slot_crc_offset(entry), table->capacity * sizeof(uint32_t));
    if (err != ESP_OK) {
        return err;
    }

    return write_zeros(table_records_offset(entry), (size_t)table->capacity * table->schema->record_size);
}

esp_err_t micro_db_flash_load_table(micro_db_table_t *table)
{
    if (!table || !table->initialized || !table->schema) {
        return ESP_ERR_INVALID_ARG;
    }

    micro_db_flash_dir_entry_t *entry = NULL;
    esp_err_t err = get_or_create_entry(table, false, &entry);
    if (err != ESP_OK) {
        return err;
    }

    micro_db_flash_table_hdr_t hdr = {0};
    err = esp_partition_read(s_part, entry->region_offset, &hdr, sizeof(hdr));
    if (err != ESP_OK) {
        return err;
    }

    if (hdr.magic != MICRO_DB_FLASH_TABLE_MAGIC ||
        hdr.version != MICRO_DB_FLASH_LAYOUT_VERSION ||
        hdr.record_size != table->schema->record_size ||
        hdr.capacity != table->capacity ||
        hdr.live_count > table->capacity) {
        ESP_LOGW(TAG,
                 "load reject table=%s key=%s hdr{magic=0x%08" PRIX32 ",ver=%u,rec=%u,cap=%u,live=%u}",
                 table->schema->name ? table->schema->name : "(unnamed)",
                 table->schema->persist_key ? table->schema->persist_key : "(none)",
                 hdr.magic,
                 (unsigned)hdr.version,
                 (unsigned)hdr.record_size,
                 (unsigned)hdr.capacity,
                 (unsigned)hdr.live_count);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG,
             "load table=%s key=%s live=%u/%u",
             table->schema->name ? table->schema->name : "(unnamed)",
             table->schema->persist_key ? table->schema->persist_key : "(none)",
             (unsigned)hdr.live_count,
             (unsigned)hdr.capacity);

    err = esp_partition_read(s_part, table_slot_used_offset(entry), table->slot_used, table_slot_used_size(table));
    if (err != ESP_OK) {
        return err;
    }

    uint32_t *slot_crc = (uint32_t *)calloc(table->capacity, sizeof(uint32_t));
    if (!slot_crc) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_partition_read(s_part, table_slot_crc_offset(entry), slot_crc, table_slot_crc_size(table));
    if (err != ESP_OK) {
        free(slot_crc);
        return err;
    }

    size_t live_slots = 0;
    size_t invalid_slots = 0;
    for (size_t i = 0; i < table->capacity; ++i) {
        const uint8_t used = table->slot_used[i];
        if (used == 0) {
            continue;
        }
        if (used == 1) {
            live_slots++;
            continue;
        }
        table->slot_used[i] = 0;
        invalid_slots++;
    }

    if (invalid_slots > 0 || live_slots != hdr.live_count) {
        ESP_LOGW(TAG,
                 "corrupt slot map table=%s key=%s invalid_slots=%u live_slots=%u hdr_live=%u",
                 table->schema->name ? table->schema->name : "(unnamed)",
                 table->schema->persist_key ? table->schema->persist_key : "(none)",
                 (unsigned)invalid_slots,
                 (unsigned)live_slots,
                 (unsigned)hdr.live_count);
        free(slot_crc);
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = esp_partition_read(s_part, table_records_offset(entry), table->records, table_records_size(table));
    if (err != ESP_OK) {
        free(slot_crc);
        return err;
    }

    const uint8_t *base = (const uint8_t *)table->records;
    for (size_t i = 0; i < table->capacity; ++i) {
        if (!table->slot_used[i]) {
            continue;
        }
        const uint8_t *record = base + (i * table->schema->record_size);
        const uint32_t expected = slot_crc[i];
        const uint32_t actual = record_checksum32(record, table->schema->record_size);
        if (expected == 0u || expected != actual) {
            ESP_LOGW(TAG,
                     "corrupt record checksum table=%s key=%s slot=%u expected=0x%08" PRIX32 " actual=0x%08" PRIX32,
                     table->schema->name ? table->schema->name : "(unnamed)",
                     table->schema->persist_key ? table->schema->persist_key : "(none)",
                     (unsigned)i,
                     expected,
                     actual);
            free(slot_crc);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    free(slot_crc);
    return ESP_OK;
}
