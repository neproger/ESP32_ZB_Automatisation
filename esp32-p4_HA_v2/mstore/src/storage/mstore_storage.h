#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mstore/mstore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal storage backend contract для Table Store.
 *
 * Table Engine владеет семантикой (key identity, generation/version, index,
 * free-list, live_count). Backend владеет только физическим хранением canonical
 * slot state (meta + key + payload) и не знает смысла key/generation.
 *
 * Контракт write:
 *   Успешный write_* означает, что canonical slot state принят backend'ом целиком
 *   (logical commit). При ошибке backend не должен оставлять состояние, в котором
 *   Table Engine считает мутацию завершённой. Engine меняет derived runtime state
 *   (index / free-list / live_count) только после успешного write_*.
 *
 * sync — внутренний инструмент backend lifecycle/flush, а не часть публичного
 * durability-контракта Table API. Caller никогда не вызывает sync; durability
 * mutating-операции определяется контрактом конкретного backend'а.
 */

typedef struct mstore_storage mstore_storage_t;

typedef struct {
    mstore_err_t (*read_meta)(const mstore_storage_t *storage, mstore_slot_t slot,
                              mstore_meta_t *out_meta);
    mstore_err_t (*read_key)(const mstore_storage_t *storage, mstore_slot_t slot,
                             void *out_key);
    mstore_err_t (*read_slot)(const mstore_storage_t *storage, mstore_slot_t slot,
                              mstore_meta_t *out_meta, void *out_key, void *out_payload);
    mstore_err_t (*write_slot)(mstore_storage_t *storage, mstore_slot_t slot,
                               const mstore_meta_t *meta, const void *key, const void *payload);
    mstore_err_t (*write_meta)(mstore_storage_t *storage, mstore_slot_t slot,
                               const mstore_meta_t *meta);
    mstore_err_t (*clear_all)(mstore_storage_t *storage);
    mstore_err_t (*sync)(mstore_storage_t *storage);
    void (*close)(mstore_storage_t *storage);
} mstore_storage_ops_t;

struct mstore_storage {
    const mstore_storage_ops_t *ops;
};

typedef struct {
    mstore_backing_t backing;
    size_t capacity;
    size_t key_size;
    size_t payload_size;
    const char *persist_key;
} mstore_storage_config_t;

mstore_err_t mstore_storage_open(const mstore_storage_config_t *config,
                                 mstore_storage_t **out_storage);

/* Backend'ы; наружу (в public API) не выходят. */
mstore_err_t mstore_storage_ram_open(const mstore_storage_config_t *config,
                                     mstore_storage_t **out_storage);
mstore_err_t mstore_storage_flash_open(const mstore_storage_config_t *config,
                                       mstore_storage_t **out_storage);
mstore_err_t mstore_storage_ram_flash_open(const mstore_storage_config_t *config,
                                           mstore_storage_t **out_storage);

static inline mstore_err_t mstore_storage_read_meta(const mstore_storage_t *storage,
                                                    mstore_slot_t slot, mstore_meta_t *out_meta) {
    return storage->ops->read_meta(storage, slot, out_meta);
}

static inline mstore_err_t mstore_storage_read_key(const mstore_storage_t *storage,
                                                   mstore_slot_t slot, void *out_key) {
    return storage->ops->read_key(storage, slot, out_key);
}

static inline mstore_err_t mstore_storage_read_slot(const mstore_storage_t *storage,
                                                    mstore_slot_t slot, mstore_meta_t *out_meta,
                                                    void *out_key, void *out_payload) {
    return storage->ops->read_slot(storage, slot, out_meta, out_key, out_payload);
}

static inline mstore_err_t mstore_storage_write_slot(mstore_storage_t *storage, mstore_slot_t slot,
                                                     const mstore_meta_t *meta, const void *key,
                                                     const void *payload) {
    return storage->ops->write_slot(storage, slot, meta, key, payload);
}

static inline mstore_err_t mstore_storage_write_meta(mstore_storage_t *storage, mstore_slot_t slot,
                                                     const mstore_meta_t *meta) {
    return storage->ops->write_meta(storage, slot, meta);
}

static inline mstore_err_t mstore_storage_clear_all(mstore_storage_t *storage) {
    return storage->ops->clear_all(storage);
}

static inline mstore_err_t mstore_storage_sync(mstore_storage_t *storage) {
    return storage->ops->sync(storage);
}

static inline void mstore_storage_close(mstore_storage_t *storage) {
    if (storage != NULL) {
        storage->ops->close(storage);
    }
}

#ifdef __cplusplus
}
#endif
