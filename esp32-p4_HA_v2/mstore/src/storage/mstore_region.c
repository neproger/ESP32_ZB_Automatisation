#include <string.h>

#include "mstore_platform.h"
#include "storage/mstore_bytes.h"
#include "storage/mstore_flash_device.h"
#include "storage/mstore_region.h"
#include "storage/mstore_storage.h"

#define MSTORE_REGION_MAGIC 0x4D534447u /* MSDG */
#define MSTORE_REGION_FORMAT_REVISION 1u
#define MSTORE_REGION_CRC_LEN 12u /* поля header до crc32 */

enum { MSTORE_REGION_FREE = 0, MSTORE_REGION_USED = 1 };

typedef struct {
    uint8_t id[16];
    size_t offset;
    size_t size;
    bool used;
} mstore_region_entry_t;

typedef struct {
    uint32_t generation;
    mstore_region_entry_t entries[MSTORE_REGION_MAX_ENTRIES];
} mstore_region_dir_t;

/* Владение живёт только в RAM: перезагрузка снимает все bind сама. */
static uint8_t s_owner_ids[MSTORE_REGION_MAX_ENTRIES][16];
static size_t s_owner_count;

static size_t slot_offset(size_t slot, size_t erase_size) {
    return slot * erase_size;
}

static bool entry_blank(const uint8_t *raw) {
    for (size_t i = 0; i < MSTORE_REGION_ENTRY_SIZE; i++) {
        if (raw[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static bool slot_erased(const uint8_t *raw) {
    for (size_t i = 0; i < MSTORE_REGION_SLOT_BYTES; i++) {
        if (raw[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static bool dir_slot_valid(const uint8_t *raw) {
    if (get_u32(raw) != MSTORE_REGION_MAGIC) {
        return false;
    }
    if (get_u16(raw + 4) != (uint16_t)MSTORE_REGION_FORMAT_REVISION) {
        return false;
    }
    if (get_u16(raw + 6) != (uint16_t)MSTORE_REGION_MAX_ENTRIES) {
        return false;
    }
    if (mstore_crc32(raw, MSTORE_REGION_CRC_LEN) != get_u32(raw + MSTORE_REGION_CRC_LEN)) {
        return false;
    }
    for (size_t i = 0; i < MSTORE_REGION_MAX_ENTRIES; i++) {
        const uint8_t *e = raw + MSTORE_REGION_HEADER_SIZE + i * MSTORE_REGION_ENTRY_SIZE;
        if (entry_blank(e)) {
            continue;
        }
        if (mstore_crc32(e, MSTORE_REGION_ENTRY_SIZE - 4) != get_u32(e + MSTORE_REGION_ENTRY_SIZE - 4)) {
            return false;
        }
        uint16_t state = get_u16(e + 24);
        if (state != MSTORE_REGION_FREE && state != MSTORE_REGION_USED) {
            return false;
        }
    }
    return true;
}

static void dir_parse(const uint8_t *raw, mstore_region_dir_t *out_dir) {
    memset(out_dir, 0, sizeof(*out_dir));
    out_dir->generation = get_u32(raw + 8);
    for (size_t i = 0; i < MSTORE_REGION_MAX_ENTRIES; i++) {
        const uint8_t *e = raw + MSTORE_REGION_HEADER_SIZE + i * MSTORE_REGION_ENTRY_SIZE;
        if (entry_blank(e)) {
            continue;
        }
        memcpy(out_dir->entries[i].id, e, 16);
        out_dir->entries[i].offset = (size_t)get_u32(e + 16);
        out_dir->entries[i].size = (size_t)get_u32(e + 20);
        out_dir->entries[i].used = get_u16(e + 24) == MSTORE_REGION_USED;
    }
}

static void dir_build(uint8_t *raw, const mstore_region_dir_t *dir, uint32_t generation) {
    memset(raw, 0xFF, MSTORE_REGION_SLOT_BYTES);
    put_u32(raw, MSTORE_REGION_MAGIC);
    put_u16(raw + 4, (uint16_t)MSTORE_REGION_FORMAT_REVISION);
    put_u16(raw + 6, (uint16_t)MSTORE_REGION_MAX_ENTRIES);
    put_u32(raw + 8, generation);
    for (size_t i = 0; i < MSTORE_REGION_MAX_ENTRIES; i++) {
        const mstore_region_entry_t *entry = &dir->entries[i];
        if (!entry->used) {
            continue; /* свободная запись остаётся erased: программировать нечего */
        }
        uint8_t *p = raw + MSTORE_REGION_HEADER_SIZE + i * MSTORE_REGION_ENTRY_SIZE;
        memcpy(p, entry->id, 16);
        put_u32(p + 16, (uint32_t)entry->offset);
        put_u32(p + 20, (uint32_t)entry->size);
        put_u16(p + 24, (uint16_t)MSTORE_REGION_USED);
        put_u16(p + 26, 0);
        put_u32(p + 28, mstore_crc32(p, MSTORE_REGION_ENTRY_SIZE - 4));
    }
    put_u32(raw + MSTORE_REGION_CRC_LEN, mstore_crc32(raw, MSTORE_REGION_CRC_LEN));
}

static bool dir_find(const mstore_region_dir_t *dir, const uint8_t id[16], size_t *out_index) {
    for (size_t i = 0; i < MSTORE_REGION_MAX_ENTRIES; i++) {
        if (!dir->entries[i].used) {
            continue;
        }
        if (memcmp(dir->entries[i].id, id, 16) == 0) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

/* Bump-аллокатор: новый регион занимает конец занятого пространства, дыры не переиспользуются. */
static size_t dir_free_offset(const mstore_region_dir_t *dir, size_t erase_size) {
    size_t end = 2 * erase_size;
    for (size_t i = 0; i < MSTORE_REGION_MAX_ENTRIES; i++) {
        if (dir->entries[i].used) {
            end += dir->entries[i].size;
        }
    }
    return end;
}

/*
 * Читает активный слот directory. NOT_FOUND — раздел ещё пустой: вызывающий
 * создаёт directory первым bind'ом. Активный слот при этом считается вторым, чтобы
 * первая запись пошла в слот 0 и ping-pong начался оттуда.
 */
static sys_error_t dir_load(const mstore_flash_device_t *partition, uint8_t *out_raw,
                             size_t *out_slot, mstore_region_dir_t *out_dir) {
    uint8_t slot0[MSTORE_REGION_SLOT_BYTES];
    uint8_t slot1[MSTORE_REGION_SLOT_BYTES];
    if (!partition->ops->read(partition->ctx, slot_offset(0, partition->erase_size), slot0,
                              sizeof(slot0))) {
        return mstore_fail(SYS_CODE_IO);
    }
    if (!partition->ops->read(partition->ctx, slot_offset(1, partition->erase_size), slot1,
                              sizeof(slot1))) {
        return mstore_fail(SYS_CODE_IO);
    }

    bool valid0 = dir_slot_valid(slot0);
    bool valid1 = dir_slot_valid(slot1);
    size_t slot;
    if (valid0 && valid1) {
        slot = (get_u32(slot0 + 8) >= get_u32(slot1 + 8)) ? 0u : 1u;
    } else if (valid0) {
        slot = 0u;
    } else if (valid1) {
        slot = 1u;
    } else {
        if (slot_erased(slot0) && slot_erased(slot1)) {
            memset(out_raw, 0xFF, MSTORE_REGION_SLOT_BYTES);
            memset(out_dir, 0, sizeof(*out_dir));
            *out_slot = 1u;
            return mstore_fail(SYS_CODE_NOT_FOUND);
        }
        /* Валидного header нет, но раздел не erased: тихо переразмечать нельзя. */
        return mstore_fail(SYS_CODE_CORRUPT);
    }

    const uint8_t *active = (slot == 0) ? slot0 : slot1;
    memcpy(out_raw, active, MSTORE_REGION_SLOT_BYTES);
    dir_parse(active, out_dir);
    *out_slot = slot;
    return SYS_OK;
}

/* Ping-pong: неактивный слот стирается и пишется, старый остаётся валидным до конца записи. */
static sys_error_t dir_commit(const mstore_flash_device_t *partition, size_t active_slot,
                              const uint8_t *raw) {
    size_t inactive = 1u - active_slot;
    size_t base = slot_offset(inactive, partition->erase_size);
    if (!partition->ops->erase(partition->ctx, base, partition->erase_size)) {
        return mstore_fail(SYS_CODE_IO);
    }
    if (!partition->ops->program(partition->ctx, base + MSTORE_REGION_HEADER_SIZE,
                                 raw + MSTORE_REGION_HEADER_SIZE,
                                 MSTORE_REGION_SLOT_BYTES - MSTORE_REGION_HEADER_SIZE)) {
        return mstore_fail(SYS_CODE_IO);
    }
    /* Header пишется последним: это точка committed-состояния directory. */
    if (!partition->ops->program(partition->ctx, base, raw, MSTORE_REGION_HEADER_SIZE)) {
        return mstore_fail(SYS_CODE_IO);
    }
    return SYS_OK;
}

static bool owner_busy(const uint8_t id[16]) {
    for (size_t i = 0; i < s_owner_count; i++) {
        if (memcmp(s_owner_ids[i], id, 16) == 0) {
            return true;
        }
    }
    return false;
}

static void owner_add(const uint8_t id[16]) {
    if (s_owner_count >= MSTORE_REGION_MAX_ENTRIES || owner_busy(id)) {
        return;
    }
    memcpy(s_owner_ids[s_owner_count], id, 16);
    s_owner_count++;
}

static void owner_remove(const uint8_t id[16]) {
    for (size_t i = 0; i < s_owner_count; i++) {
        if (memcmp(s_owner_ids[i], id, 16) != 0) {
            continue;
        }
        memmove(s_owner_ids[i], s_owner_ids[i + 1], 16 * (s_owner_count - i - 1));
        s_owner_count--;
        return;
    }
}

/* Владение имеет смысл только пока регион есть в directory: после смены раздела оно снимается. */
static void owner_prune(const mstore_region_dir_t *dir) {
    size_t kept = 0;
    for (size_t i = 0; i < s_owner_count; i++) {
        size_t index = 0;
        if (dir_find(dir, s_owner_ids[i], &index)) {
            memcpy(s_owner_ids[kept], s_owner_ids[i], 16);
            kept++;
        }
    }
    s_owner_count = kept;
}

static bool region_view_in_bounds(const mstore_region_view_ctx_t *ctx, size_t offset, size_t len) {
    return offset <= ctx->size && len <= ctx->size - offset;
}

static bool region_view_read(void *ctx, size_t offset, void *dst, size_t len) {
    mstore_region_view_ctx_t *c = ctx;
    if (!region_view_in_bounds(c, offset, len)) {
        return false;
    }
    return c->parent->ops->read(c->parent->ctx, c->base_offset + offset, dst, len);
}

static bool region_view_program(void *ctx, size_t offset, const void *src, size_t len) {
    mstore_region_view_ctx_t *c = ctx;
    if (!region_view_in_bounds(c, offset, len)) {
        return false;
    }
    return c->parent->ops->program(c->parent->ctx, c->base_offset + offset, src, len);
}

static bool region_view_erase(void *ctx, size_t offset, size_t len) {
    mstore_region_view_ctx_t *c = ctx;
    if (!region_view_in_bounds(c, offset, len)) {
        return false;
    }
    return c->parent->ops->erase(c->parent->ctx, c->base_offset + offset, len);
}

static const mstore_flash_device_ops_t MSTORE_REGION_VIEW_OPS = {
    .read = region_view_read,
    .program = region_view_program,
    .erase = region_view_erase,
};

static sys_error_t partition_geometry_ok(const mstore_flash_device_t *partition) {
    if (partition->erase_size < MSTORE_REGION_SLOT_BYTES) {
        return mstore_fail(SYS_CODE_INVALID_SIZE); /* слот directory не влезает в erase-блок */
    }
    if (partition->erase_size > SIZE_MAX / 2 || partition->size < 2 * partition->erase_size) {
        return mstore_fail(SYS_CODE_INVALID_SIZE);
    }
    return SYS_OK;
}

static sys_error_t dir_read(const mstore_flash_device_t *partition, mstore_region_dir_t *out_dir,
                             uint8_t *out_raw, size_t *out_slot) {
    sys_error_t err = partition_geometry_ok(partition);
    if (sys_failed(err)) {
        return err;
    }
    return dir_load(partition, out_raw, out_slot, out_dir);
}

sys_error_t mstore_region_bind(const char *persist_key, size_t capacity, size_t key_size,
                                size_t payload_size, mstore_region_view_t *out_view) {
    if (persist_key == NULL || persist_key[0] == '\0' || out_view == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    const mstore_flash_device_t *partition = mstore_platform_flash_device();
    if (partition == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }

    uint8_t raw[MSTORE_REGION_SLOT_BYTES];
    mstore_region_dir_t dir;
    size_t slot = 0;
    sys_error_t err = dir_read(partition, &dir, raw, &slot);
    if (sys_failed(err) && !sys_is(err, SYS_CODE_NOT_FOUND)) {
        return err;
    }

    size_t region_size = 0;
    err = mstore_storage_flash_region_size(capacity, key_size, payload_size, partition->erase_size,
                                           &region_size);
    if (sys_failed(err)) {
        return err;
    }

    uint8_t id[16];
    mstore_persist_id(persist_key, id);
    owner_prune(&dir);

    size_t index = 0;
    size_t region_offset;
    if (dir_find(&dir, id, &index)) {
        if (owner_busy(id)) {
            return mstore_fail(SYS_CODE_INVALID_STATE);
        }
        if (dir.entries[index].size != region_size) {
            return mstore_fail(SYS_CODE_INVALID_SIZE); /* v1: регион не растёт */
        }
        region_offset = dir.entries[index].offset;
        if (region_offset < 2 * partition->erase_size ||
            region_offset % partition->erase_size != 0 || region_offset > partition->size ||
            region_size > partition->size - region_offset) {
            return mstore_fail(SYS_CODE_CORRUPT);
        }
    } else {
        for (index = 0; index < MSTORE_REGION_MAX_ENTRIES && dir.entries[index].used; index++) {
        }
        if (index == MSTORE_REGION_MAX_ENTRIES) {
            return mstore_fail(SYS_CODE_NO_SPACE);
        }
        region_offset = dir_free_offset(&dir, partition->erase_size);
        if (region_offset > partition->size || region_size > partition->size - region_offset) {
            return mstore_fail(SYS_CODE_INVALID_SIZE); /* free tail меньше региона */
        }
        dir.entries[index].used = true;
        memcpy(dir.entries[index].id, id, 16);
        dir.entries[index].offset = region_offset;
        dir.entries[index].size = region_size;
        uint32_t generation = dir.generation + 1;
        dir_build(raw, &dir, generation);
        err = dir_commit(partition, slot, raw);
        if (sys_failed(err)) {
            return err;
        }
    }

    owner_add(id);
    out_view->ctx.parent = partition;
    out_view->ctx.base_offset = region_offset;
    out_view->ctx.size = region_size;
    out_view->device.ops = &MSTORE_REGION_VIEW_OPS;
    out_view->device.ctx = &out_view->ctx;
    out_view->device.size = region_size;
    out_view->device.erase_size = partition->erase_size;
    return SYS_OK;
}

void mstore_region_release(const char *persist_key) {
    if (persist_key == NULL || persist_key[0] == '\0') {
        return;
    }
    uint8_t id[16];
    mstore_persist_id(persist_key, id);
    owner_remove(id);
}

sys_error_t mstore_region_lookup(const char *persist_key, size_t *out_offset, size_t *out_size) {
    if (persist_key == NULL || out_offset == NULL || out_size == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
    }
    const mstore_flash_device_t *partition = mstore_platform_flash_device();
    if (partition == NULL) {
        return mstore_fail(SYS_CODE_INVALID_STATE);
    }

    uint8_t raw[MSTORE_REGION_SLOT_BYTES];
    mstore_region_dir_t dir;
    size_t slot = 0;
    sys_error_t err = dir_read(partition, &dir, raw, &slot);
    if (sys_failed(err)) {
        return err;
    }

    uint8_t id[16];
    mstore_persist_id(persist_key, id);
    size_t index = 0;
    if (!dir_find(&dir, id, &index)) {
        return mstore_fail(SYS_CODE_NOT_FOUND);
    }
    *out_offset = dir.entries[index].offset;
    *out_size = dir.entries[index].size;
    return SYS_OK;
}
