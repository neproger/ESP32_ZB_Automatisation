#include <string.h>

#include "mstore_platform.h"
#include "storage/mstore_storage.h"

typedef struct {
    mstore_storage_t base;
    size_t capacity;
    size_t key_size;
    size_t payload_size;
    size_t slot_size;
    uint8_t *slots;
} mstore_ram_storage_t;

static mstore_ram_storage_t *mstore_ram_of(mstore_storage_t *storage) {
    return (mstore_ram_storage_t *)storage;
}

static const mstore_ram_storage_t *mstore_ram_of_const(const mstore_storage_t *storage) {
    return (const mstore_ram_storage_t *)storage;
}

static size_t mstore_ram_slot_size(size_t key_size, size_t payload_size) {
    const size_t raw = sizeof(mstore_meta_t) + key_size + payload_size;
    const size_t alignment = _Alignof(mstore_meta_t);
    return (raw + alignment - 1) & ~(alignment - 1);
}

static uint8_t *mstore_ram_slot(const mstore_ram_storage_t *st, mstore_slot_t slot) {
    return st->slots + (size_t)slot * st->slot_size;
}

static mstore_meta_t *mstore_ram_meta(const mstore_ram_storage_t *st, mstore_slot_t slot) {
    return (mstore_meta_t *)mstore_ram_slot(st, slot);
}

static void *mstore_ram_key(const mstore_ram_storage_t *st, mstore_slot_t slot) {
    return mstore_ram_slot(st, slot) + sizeof(mstore_meta_t);
}

static void *mstore_ram_payload(const mstore_ram_storage_t *st, mstore_slot_t slot) {
    return (uint8_t *)mstore_ram_key(st, slot) + st->key_size;
}

static mstore_err_t ram_read_meta(const mstore_storage_t *storage, mstore_slot_t slot,
                                  mstore_meta_t *out_meta) {
    *out_meta = *mstore_ram_meta(mstore_ram_of_const(storage), slot);
    return MSTORE_OK;
}

static mstore_err_t ram_read_key(const mstore_storage_t *storage, mstore_slot_t slot,
                                 void *out_key) {
    const mstore_ram_storage_t *st = mstore_ram_of_const(storage);
    memcpy(out_key, mstore_ram_key(st, slot), st->key_size);
    return MSTORE_OK;
}

static mstore_err_t ram_read_slot(const mstore_storage_t *storage, mstore_slot_t slot,
                                  mstore_meta_t *out_meta, void *out_key, void *out_payload) {
    const mstore_ram_storage_t *st = mstore_ram_of_const(storage);
    *out_meta = *mstore_ram_meta(st, slot);
    memcpy(out_key, mstore_ram_key(st, slot), st->key_size);
    memcpy(out_payload, mstore_ram_payload(st, slot), st->payload_size);
    return MSTORE_OK;
}

static mstore_err_t ram_write_slot(mstore_storage_t *storage, mstore_slot_t slot,
                                   const mstore_meta_t *meta, const void *key,
                                   const void *payload) {
    mstore_ram_storage_t *st = mstore_ram_of(storage);
    *mstore_ram_meta(st, slot) = *meta;
    memcpy(mstore_ram_key(st, slot), key, st->key_size);
    memcpy(mstore_ram_payload(st, slot), payload, st->payload_size);
    return MSTORE_OK;
}

static mstore_err_t ram_write_meta(mstore_storage_t *storage, mstore_slot_t slot,
                                   const mstore_meta_t *meta) {
    mstore_ram_storage_t *st = mstore_ram_of(storage);
    *mstore_ram_meta(st, slot) = *meta;
    return MSTORE_OK;
}

static mstore_err_t ram_clear_all(mstore_storage_t *storage) {
    mstore_ram_storage_t *st = mstore_ram_of(storage);
    for (size_t i = 0; i < st->capacity; i++) {
        mstore_ram_meta(st, (mstore_slot_t)i)->used = false;
    }
    return MSTORE_OK;
}

static mstore_err_t ram_sync(mstore_storage_t *storage) {
    (void)storage;
    return MSTORE_OK;
}

static void ram_close(mstore_storage_t *storage) {
    mstore_ram_storage_t *st = mstore_ram_of(storage);
    mstore_platform_free(st->slots);
    mstore_platform_free(st);
}

static const mstore_storage_ops_t MSTORE_RAM_OPS = {
    .read_meta = ram_read_meta,
    .read_key = ram_read_key,
    .read_slot = ram_read_slot,
    .write_slot = ram_write_slot,
    .write_meta = ram_write_meta,
    .clear_all = ram_clear_all,
    .sync = ram_sync,
    .close = ram_close,
};

mstore_err_t mstore_storage_ram_open(const mstore_storage_config_t *config,
                                     mstore_storage_t **out_storage) {
    if (config->capacity == 0 || config->key_size == 0) {
        return MSTORE_INVALID_SIZE;
    }

    mstore_ram_storage_t *st = mstore_platform_alloc(sizeof(*st));
    if (st == NULL) {
        return MSTORE_NO_MEM;
    }
    memset(st, 0, sizeof(*st));

    st->base.ops = &MSTORE_RAM_OPS;
    st->capacity = config->capacity;
    st->key_size = config->key_size;
    st->payload_size = config->payload_size;
    st->slot_size = mstore_ram_slot_size(config->key_size, config->payload_size);

    st->slots = mstore_platform_alloc(st->slot_size * st->capacity);
    if (st->slots == NULL) {
        mstore_platform_free(st);
        return MSTORE_NO_MEM;
    }
    memset(st->slots, 0, st->slot_size * st->capacity);

    *out_storage = &st->base;
    return MSTORE_OK;
}
