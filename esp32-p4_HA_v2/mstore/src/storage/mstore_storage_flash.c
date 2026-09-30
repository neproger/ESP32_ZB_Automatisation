#include <string.h>

#include "mstore_platform.h"
#include "storage/mstore_flash_device.h"
#include "storage/mstore_storage.h"

#define MSTORE_FLASH_HEADER_MAGIC 0x4D535442u   /* MSTB */
#define MSTORE_FLASH_RECORD_MAGIC 0x5452u /* "RT", record prefix */
#define MSTORE_FLASH_COMMIT_MARKER 0x4D53434Du  /* MSCM */
#define MSTORE_FLASH_FORMAT_REVISION 1u
#define MSTORE_FLASH_HEADER_SIZE 64u
#define MSTORE_FLASH_HEADER_CRC_LEN 36u
#define MSTORE_FLASH_RECORD_FIXED 16u
#define MSTORE_FLASH_RECORD_TAIL 8u /* crc32 + commit_marker */

enum { MSTORE_FLASH_KIND_SET = 1, MSTORE_FLASH_KIND_META = 2 };

typedef struct {
    uint32_t offset; /* 0 = записи не было */
    uint8_t kind;
} mstore_flash_latest_t;

typedef struct {
    mstore_storage_t base;
    size_t capacity;
    size_t key_size;
    size_t payload_size;
    const mstore_flash_device_t *device;

    size_t header_size;
    size_t bank_size;
    size_t record_capacity;

    size_t active_bank;
    uint32_t active_seq;
    size_t write_offset;
    bool needs_checkpoint;

    char *persist_key;
    mstore_flash_latest_t *latest;
    uint8_t *record_buf;
} mstore_flash_storage_t;

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t mstore_crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

static uint64_t mstore_fnv1a64(const char *text, uint64_t seed) {
    uint64_t hash = seed;
    for (const unsigned char *p = (const unsigned char *)text; *p != 0; p++) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void mstore_persist_id(const char *key, uint8_t out[16]) {
    uint64_t a = mstore_fnv1a64(key, 1469598103934665603ULL);
    uint64_t b = mstore_fnv1a64(key, 1099511628211ULL);
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(a >> (8 * i));
        out[8 + i] = (uint8_t)(b >> (8 * i));
    }
}

static bool dev_read(const mstore_flash_storage_t *st, size_t offset, void *dst, size_t len) {
    return st->device->ops->read(st->device->ctx, offset, dst, len);
}

static bool dev_program(const mstore_flash_storage_t *st, size_t offset, const void *src, size_t len) {
    return st->device->ops->program(st->device->ctx, offset, src, len);
}

static bool dev_erase(const mstore_flash_storage_t *st, size_t offset, size_t len) {
    return st->device->ops->erase(st->device->ctx, offset, len);
}

static size_t bank_offset(const mstore_flash_storage_t *st, size_t bank) {
    return bank * st->bank_size;
}

static size_t record_len(const mstore_flash_storage_t *st, uint8_t kind) {
    size_t len = MSTORE_FLASH_RECORD_FIXED + MSTORE_FLASH_RECORD_TAIL;
    if (kind == MSTORE_FLASH_KIND_SET) {
        len += st->key_size + st->payload_size;
    }
    return len;
}

static bool header_read(const mstore_flash_storage_t *st, size_t bank, uint8_t *hdr) {
    if (!dev_read(st, bank_offset(st, bank), hdr, st->header_size)) {
        return false;
    }
    if (get_u32(hdr) != MSTORE_FLASH_HEADER_MAGIC) {
        return false;
    }
    if (get_u16(hdr + 4) != (uint16_t)MSTORE_FLASH_FORMAT_REVISION) {
        return false;
    }
    if ((size_t)get_u16(hdr + 6) != st->header_size) {
        return false;
    }
    if (mstore_crc32(hdr, MSTORE_FLASH_HEADER_CRC_LEN) != get_u32(hdr + MSTORE_FLASH_HEADER_CRC_LEN)) {
        return false;
    }
    return true;
}

static bool header_matches(const mstore_flash_storage_t *st, const uint8_t *hdr) {
    uint8_t id[16];
    if (get_u32(hdr + 12) != (uint32_t)st->capacity) {
        return false;
    }
    if ((size_t)get_u16(hdr + 16) != st->key_size || (size_t)get_u16(hdr + 18) != st->payload_size) {
        return false;
    }
    mstore_persist_id(st->persist_key, id);
    return memcmp(hdr + 20, id, sizeof(id)) == 0;
}

static void header_build(const mstore_flash_storage_t *st, uint8_t *hdr, uint32_t seq) {
    uint8_t id[16];
    memset(hdr, 0xFF, st->header_size);
    put_u32(hdr, MSTORE_FLASH_HEADER_MAGIC);
    put_u16(hdr + 4, (uint16_t)MSTORE_FLASH_FORMAT_REVISION);
    put_u16(hdr + 6, (uint16_t)st->header_size);
    put_u32(hdr + 8, seq);
    put_u32(hdr + 12, (uint32_t)st->capacity);
    put_u16(hdr + 16, (uint16_t)st->key_size);
    put_u16(hdr + 18, (uint16_t)st->payload_size);
    mstore_persist_id(st->persist_key, id);
    memcpy(hdr + 20, id, sizeof(id));
    put_u32(hdr + MSTORE_FLASH_HEADER_CRC_LEN, mstore_crc32(hdr, MSTORE_FLASH_HEADER_CRC_LEN));
}

static void build_record(const mstore_flash_storage_t *st, uint8_t kind, uint32_t slot,
                         const mstore_meta_t *meta, const void *key, const void *payload, size_t len) {
    uint8_t *r = st->record_buf;
    size_t crc_off = MSTORE_FLASH_RECORD_FIXED;
    memset(r, 0xFF, len);
    put_u16(r, MSTORE_FLASH_RECORD_MAGIC);
    r[2] = kind;
    r[3] = meta->used ? 1 : 0;
    put_u32(r + 4, slot);
    put_u32(r + 8, meta->generation);
    put_u32(r + 12, meta->version);
    if (kind == MSTORE_FLASH_KIND_SET) {
        memcpy(r + MSTORE_FLASH_RECORD_FIXED, key, st->key_size);
        memcpy(r + MSTORE_FLASH_RECORD_FIXED + st->key_size, payload, st->payload_size);
        crc_off += st->key_size + st->payload_size;
    }
    put_u32(r + crc_off, mstore_crc32(r, crc_off));
    put_u32(r + crc_off + 4, MSTORE_FLASH_COMMIT_MARKER);
}

static mstore_err_t program_record(const mstore_flash_storage_t *st, size_t bank, size_t offset,
                                   const uint8_t *record, size_t len) {
    size_t base = bank_offset(st, bank) + offset;
    if (!dev_program(st, base, record, len - 4)) {
        return MSTORE_IO;
    }
    if (!dev_program(st, base + len - 4, record + len - 4, 4)) {
        return MSTORE_IO;
    }
    return MSTORE_OK;
}

static mstore_err_t flash_scan(mstore_flash_storage_t *st, bool *out_torn) {
    size_t offset = st->header_size;
    bool torn = false;
    uint8_t prefix[MSTORE_FLASH_RECORD_FIXED];

    while (offset + MSTORE_FLASH_RECORD_FIXED <= st->bank_size) {
        if (!dev_read(st, bank_offset(st, st->active_bank) + offset, prefix, sizeof(prefix))) {
            return MSTORE_IO;
        }
        if (get_u16(prefix) != MSTORE_FLASH_RECORD_MAGIC) {
            bool erased = true;
            for (size_t i = 0; i < sizeof(prefix); i++) {
                if (prefix[i] != 0xFF) {
                    erased = false;
                    break;
                }
            }
            torn = !erased;
            break;
        }
        uint8_t kind = prefix[2];
        if (kind != MSTORE_FLASH_KIND_SET && kind != MSTORE_FLASH_KIND_META) {
            torn = true;
            break;
        }
        size_t len = record_len(st, kind);
        if (offset + len > st->bank_size) {
            torn = true;
            break;
        }
        uint8_t marker[4];
        if (!dev_read(st, bank_offset(st, st->active_bank) + offset + len - 4, marker, sizeof(marker))) {
            return MSTORE_IO;
        }
        if (get_u32(marker) != MSTORE_FLASH_COMMIT_MARKER) {
            torn = true;
            break;
        }
        if (len > st->record_capacity) {
            return MSTORE_CORRUPT;
        }
        if (!dev_read(st, bank_offset(st, st->active_bank) + offset, st->record_buf, len)) {
            return MSTORE_IO;
        }
        if (mstore_crc32(st->record_buf, len - 8) != get_u32(st->record_buf + len - 8)) {
            return MSTORE_CORRUPT;
        }
        uint32_t slot = get_u32(prefix + 4);
        if (slot >= st->capacity) {
            return MSTORE_CORRUPT;
        }
        st->latest[slot].offset = (uint32_t)offset;
        st->latest[slot].kind = kind;
        offset += len;
    }

    st->write_offset = offset;
    *out_torn = torn;
    return MSTORE_OK;
}

static mstore_err_t flash_compact(mstore_flash_storage_t *st, bool clear) {
    size_t inactive = 1 - st->active_bank;
    if (!dev_erase(st, bank_offset(st, inactive), st->bank_size)) {
        return MSTORE_IO;
    }

    mstore_flash_latest_t *new_latest = mstore_platform_alloc(sizeof(*new_latest) * st->capacity);
    if (new_latest == NULL) {
        return MSTORE_NO_MEM;
    }
    memset(new_latest, 0, sizeof(*new_latest) * st->capacity);

    size_t cursor = st->header_size;
    for (size_t slot = 0; slot < st->capacity; slot++) {
        mstore_flash_latest_t entry = st->latest[slot];
        if (entry.offset == 0) {
            continue;
        }
        uint8_t prefix[MSTORE_FLASH_RECORD_FIXED];
        if (!dev_read(st, bank_offset(st, st->active_bank) + entry.offset, prefix, sizeof(prefix))) {
            mstore_platform_free(new_latest);
            return MSTORE_IO;
        }
        mstore_meta_t meta;
        meta.used = prefix[3] != 0;
        meta.generation = get_u32(prefix + 8);
        meta.version = get_u32(prefix + 12);
        uint8_t kind = entry.kind;
        if (clear) {
            meta.used = false;
            kind = MSTORE_FLASH_KIND_META;
        }

        size_t len = record_len(st, kind);
        if (cursor + len > st->bank_size) {
            mstore_platform_free(new_latest);
            return MSTORE_CORRUPT;
        }

        uint8_t *r = st->record_buf;
        size_t crc_off = MSTORE_FLASH_RECORD_FIXED;
        memset(r, 0xFF, len);
        put_u16(r, MSTORE_FLASH_RECORD_MAGIC);
        r[2] = kind;
        r[3] = meta.used ? 1 : 0;
        put_u32(r + 4, (uint32_t)slot);
        put_u32(r + 8, meta.generation);
        put_u32(r + 12, meta.version);
        if (kind == MSTORE_FLASH_KIND_SET) {
            if (!dev_read(st, bank_offset(st, st->active_bank) + entry.offset + MSTORE_FLASH_RECORD_FIXED,
                          r + MSTORE_FLASH_RECORD_FIXED, st->key_size + st->payload_size)) {
                mstore_platform_free(new_latest);
                return MSTORE_IO;
            }
            crc_off += st->key_size + st->payload_size;
        }
        put_u32(r + crc_off, mstore_crc32(r, crc_off));
        put_u32(r + crc_off + 4, MSTORE_FLASH_COMMIT_MARKER);

        mstore_err_t err = program_record(st, inactive, cursor, r, len);
        if (err != MSTORE_OK) {
            mstore_platform_free(new_latest);
            return err;
        }
        new_latest[slot].offset = (uint32_t)cursor;
        new_latest[slot].kind = kind;
        cursor += len;
    }

    uint8_t hdr[MSTORE_FLASH_HEADER_SIZE];
    header_build(st, hdr, st->active_seq + 1);
    if (!dev_program(st, bank_offset(st, inactive), hdr, st->header_size)) {
        mstore_platform_free(new_latest);
        return MSTORE_IO;
    }

    mstore_platform_free(st->latest);
    st->latest = new_latest;
    size_t old_bank = st->active_bank;
    st->active_bank = inactive;
    st->active_seq++;
    st->write_offset = cursor;
    st->needs_checkpoint = false;

    (void)dev_erase(st, bank_offset(st, old_bank), st->bank_size);
    return MSTORE_OK;
}

static mstore_err_t flash_append(mstore_flash_storage_t *st, uint32_t slot, uint8_t kind,
                                 const mstore_meta_t *meta, const void *key, const void *payload) {
    if (st->needs_checkpoint) {
        mstore_err_t err = flash_compact(st, false);
        if (err != MSTORE_OK) {
            return err;
        }
    }
    size_t len = record_len(st, kind);
    if (st->write_offset + len > st->bank_size) {
        mstore_err_t err = flash_compact(st, false);
        if (err != MSTORE_OK) {
            return err;
        }
    }
    if (st->write_offset + len > st->bank_size) {
        return MSTORE_NO_SPACE;
    }
    build_record(st, kind, slot, meta, key, payload, len);
    mstore_err_t err = program_record(st, st->active_bank, st->write_offset, st->record_buf, len);
    if (err != MSTORE_OK) {
        return err;
    }
    st->latest[slot].offset = (uint32_t)st->write_offset;
    st->latest[slot].kind = kind;
    st->write_offset += len;
    return MSTORE_OK;
}

static mstore_err_t flash_read_meta(const mstore_storage_t *base, mstore_slot_t slot,
                                    mstore_meta_t *out_meta) {
    const mstore_flash_storage_t *st = (const mstore_flash_storage_t *)base;
    mstore_flash_latest_t entry = st->latest[slot];
    if (entry.offset == 0) {
        out_meta->used = false;
        out_meta->generation = 0;
        out_meta->version = 0;
        return MSTORE_OK;
    }
    uint8_t prefix[MSTORE_FLASH_RECORD_FIXED];
    if (!dev_read(st, bank_offset(st, st->active_bank) + entry.offset, prefix, sizeof(prefix))) {
        return MSTORE_IO;
    }
    out_meta->used = prefix[3] != 0;
    out_meta->generation = get_u32(prefix + 8);
    out_meta->version = get_u32(prefix + 12);
    return MSTORE_OK;
}

static mstore_err_t flash_read_key(const mstore_storage_t *base, mstore_slot_t slot, void *out_key) {
    const mstore_flash_storage_t *st = (const mstore_flash_storage_t *)base;
    mstore_flash_latest_t entry = st->latest[slot];
    memset(out_key, 0, st->key_size);
    if (entry.offset == 0 || entry.kind != MSTORE_FLASH_KIND_SET) {
        return MSTORE_OK;
    }
    if (!dev_read(st, bank_offset(st, st->active_bank) + entry.offset + MSTORE_FLASH_RECORD_FIXED,
                  out_key, st->key_size)) {
        return MSTORE_IO;
    }
    return MSTORE_OK;
}

static mstore_err_t flash_read_slot(const mstore_storage_t *base, mstore_slot_t slot,
                                    mstore_meta_t *out_meta, void *out_key, void *out_payload) {
    const mstore_flash_storage_t *st = (const mstore_flash_storage_t *)base;
    mstore_flash_latest_t entry = st->latest[slot];
    if (entry.offset == 0) {
        out_meta->used = false;
        out_meta->generation = 0;
        out_meta->version = 0;
        memset(out_key, 0, st->key_size);
        memset(out_payload, 0, st->payload_size);
        return MSTORE_OK;
    }
    uint8_t prefix[MSTORE_FLASH_RECORD_FIXED];
    size_t base_offset = bank_offset(st, st->active_bank) + entry.offset;
    if (!dev_read(st, base_offset, prefix, sizeof(prefix))) {
        return MSTORE_IO;
    }
    out_meta->used = prefix[3] != 0;
    out_meta->generation = get_u32(prefix + 8);
    out_meta->version = get_u32(prefix + 12);
    if (entry.kind == MSTORE_FLASH_KIND_SET) {
        if (!dev_read(st, base_offset + MSTORE_FLASH_RECORD_FIXED, out_key, st->key_size)) {
            return MSTORE_IO;
        }
        if (!dev_read(st, base_offset + MSTORE_FLASH_RECORD_FIXED + st->key_size, out_payload,
                      st->payload_size)) {
            return MSTORE_IO;
        }
    } else {
        memset(out_key, 0, st->key_size);
        memset(out_payload, 0, st->payload_size);
    }
    return MSTORE_OK;
}

static mstore_err_t flash_write_slot(mstore_storage_t *base, mstore_slot_t slot,
                                     const mstore_meta_t *meta, const void *key, const void *payload) {
    return flash_append((mstore_flash_storage_t *)base, slot, MSTORE_FLASH_KIND_SET, meta, key, payload);
}

static mstore_err_t flash_write_meta(mstore_storage_t *base, mstore_slot_t slot,
                                     const mstore_meta_t *meta) {
    return flash_append((mstore_flash_storage_t *)base, slot, MSTORE_FLASH_KIND_META, meta, NULL, NULL);
}

static mstore_err_t flash_clear_all(mstore_storage_t *base) {
    return flash_compact((mstore_flash_storage_t *)base, true);
}

static mstore_err_t flash_sync(mstore_storage_t *base) {
    (void)base;
    return MSTORE_OK;
}

static void flash_close(mstore_storage_t *base) {
    mstore_flash_storage_t *st = (mstore_flash_storage_t *)base;
    if (st->record_buf != NULL) {
        mstore_platform_free(st->record_buf);
    }
    if (st->latest != NULL) {
        mstore_platform_free(st->latest);
    }
    if (st->persist_key != NULL) {
        mstore_platform_free(st->persist_key);
    }
    mstore_platform_free(st);
}

static const mstore_storage_ops_t MSTORE_FLASH_OPS = {
    .read_meta = flash_read_meta,
    .read_key = flash_read_key,
    .read_slot = flash_read_slot,
    .write_slot = flash_write_slot,
    .write_meta = flash_write_meta,
    .clear_all = flash_clear_all,
    .sync = flash_sync,
    .close = flash_close,
};

mstore_err_t mstore_storage_flash_open(const mstore_storage_config_t *config,
                                       mstore_storage_t **out_storage) {
    if (config->backing != MSTORE_BACKING_FLASH) {
        return MSTORE_INVALID_ARG;
    }
    if (config->persist_key == NULL || config->persist_key[0] == '\0') {
        return MSTORE_INVALID_ARG;
    }
    if (config->capacity == 0 || config->key_size == 0) {
        return MSTORE_INVALID_SIZE;
    }
    if (config->key_size > 0xFFFF || config->payload_size > 0xFFFF) {
        return MSTORE_INVALID_SIZE;
    }

    const mstore_flash_device_t *device = mstore_platform_flash_device();
    if (device == NULL) {
        return MSTORE_INVALID_STATE;
    }
    if (device->erase_size == 0 || device->size < 2 * device->erase_size) {
        return MSTORE_INVALID_SIZE;
    }

    size_t bank_size = (device->size / 2) & ~(device->erase_size - 1);
    size_t max_record =
        MSTORE_FLASH_RECORD_FIXED + config->key_size + config->payload_size + MSTORE_FLASH_RECORD_TAIL;
    if (bank_size < MSTORE_FLASH_HEADER_SIZE + config->capacity * max_record) {
        return MSTORE_INVALID_SIZE;
    }

    mstore_flash_storage_t *st = mstore_platform_alloc(sizeof(*st));
    if (st == NULL) {
        return MSTORE_NO_MEM;
    }
    memset(st, 0, sizeof(*st));
    st->base.ops = &MSTORE_FLASH_OPS;
    st->capacity = config->capacity;
    st->key_size = config->key_size;
    st->payload_size = config->payload_size;
    st->device = device;
    st->header_size = MSTORE_FLASH_HEADER_SIZE;
    st->bank_size = bank_size;
    st->record_capacity = max_record;

    size_t key_len = strlen(config->persist_key);
    st->persist_key = mstore_platform_alloc(key_len + 1);
    st->latest = mstore_platform_alloc(sizeof(*st->latest) * st->capacity);
    st->record_buf = mstore_platform_alloc(max_record);
    if (st->persist_key == NULL || st->latest == NULL || st->record_buf == NULL) {
        flash_close(&st->base);
        return MSTORE_NO_MEM;
    }
    memcpy(st->persist_key, config->persist_key, key_len + 1);
    memset(st->latest, 0, sizeof(*st->latest) * st->capacity);

    uint8_t hdr0[MSTORE_FLASH_HEADER_SIZE];
    uint8_t hdr1[MSTORE_FLASH_HEADER_SIZE];
    bool valid0 = header_read(st, 0, hdr0);
    bool valid1 = header_read(st, 1, hdr1);

    int active = -1;
    if (valid0 && valid1) {
        active = (get_u32(hdr0 + 8) >= get_u32(hdr1 + 8)) ? 0 : 1;
    } else if (valid0) {
        active = 0;
    } else if (valid1) {
        active = 1;
    }

    mstore_err_t err = MSTORE_OK;
    if (active < 0) {
        if (!dev_erase(st, 0, st->bank_size)) {
            err = MSTORE_IO;
        } else {
            uint8_t hdr[MSTORE_FLASH_HEADER_SIZE];
            header_build(st, hdr, 1);
            if (!dev_program(st, 0, hdr, st->header_size)) {
                err = MSTORE_IO;
            }
        }
        if (err == MSTORE_OK) {
            st->active_bank = 0;
            st->active_seq = 1;
            st->write_offset = st->header_size;
            st->needs_checkpoint = false;
        }
    } else {
        const uint8_t *hdr = (active == 0) ? hdr0 : hdr1;
        if (!header_matches(st, hdr)) {
            err = MSTORE_INVALID_STATE;
        } else {
            st->active_bank = (size_t)active;
            st->active_seq = get_u32(hdr + 8);
            bool torn = false;
            err = flash_scan(st, &torn);
            if (err == MSTORE_OK) {
                st->needs_checkpoint = torn;
            }
        }
    }

    if (err != MSTORE_OK) {
        flash_close(&st->base);
        return err;
    }
    *out_storage = &st->base;
    return MSTORE_OK;
}
