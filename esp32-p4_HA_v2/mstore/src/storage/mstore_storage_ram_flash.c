#include "mstore_platform.h"
#include "storage/mstore_storage.h"

/*
 * Composite backend: RAM — working/hot storage, FLASH — durable backing.
 *
 * Reads идут из RAM. Мутации write-through: сначала durable FLASH commit, затем
 * RAM. Если FLASH write завершился ошибкой, RAM не меняется — оба backend'а
 * остаются на последнем committed состоянии. Публичный API и Table Engine
 * семантику не меняют.
 */

typedef struct {
    mstore_storage_t base;
    mstore_storage_t *ram;
    mstore_storage_t *flash;
    size_t capacity;
    size_t key_size;
    size_t payload_size;
} mstore_ram_flash_t;

static mstore_err_t rf_read_meta(const mstore_storage_t *base, mstore_slot_t slot,
                                 mstore_meta_t *out_meta) {
    return mstore_storage_read_meta(((const mstore_ram_flash_t *)base)->ram, slot, out_meta);
}

static mstore_err_t rf_read_key(const mstore_storage_t *base, mstore_slot_t slot, void *out_key) {
    return mstore_storage_read_key(((const mstore_ram_flash_t *)base)->ram, slot, out_key);
}

static mstore_err_t rf_read_slot(const mstore_storage_t *base, mstore_slot_t slot,
                                 mstore_meta_t *out_meta, void *out_key, void *out_payload) {
    return mstore_storage_read_slot(((const mstore_ram_flash_t *)base)->ram, slot, out_meta, out_key,
                                    out_payload);
}

static mstore_err_t rf_write_slot(mstore_storage_t *base, mstore_slot_t slot,
                                  const mstore_meta_t *meta, const void *key, const void *payload) {
    mstore_ram_flash_t *st = (mstore_ram_flash_t *)base;
    mstore_err_t err = mstore_storage_write_slot(st->flash, slot, meta, key, payload);
    if (err != MSTORE_OK) {
        return err;
    }
    return mstore_storage_write_slot(st->ram, slot, meta, key, payload);
}

static mstore_err_t rf_write_meta(mstore_storage_t *base, mstore_slot_t slot,
                                  const mstore_meta_t *meta) {
    mstore_ram_flash_t *st = (mstore_ram_flash_t *)base;
    mstore_err_t err = mstore_storage_write_meta(st->flash, slot, meta);
    if (err != MSTORE_OK) {
        return err;
    }
    return mstore_storage_write_meta(st->ram, slot, meta);
}

static mstore_err_t rf_clear_all(mstore_storage_t *base) {
    mstore_ram_flash_t *st = (mstore_ram_flash_t *)base;
    mstore_err_t err = mstore_storage_clear_all(st->flash);
    if (err != MSTORE_OK) {
        return err;
    }
    return mstore_storage_clear_all(st->ram);
}

static mstore_err_t rf_sync(mstore_storage_t *base) {
    (void)base;
    return MSTORE_OK;
}

static void rf_close(mstore_storage_t *base) {
    mstore_ram_flash_t *st = (mstore_ram_flash_t *)base;
    if (st->ram != NULL) {
        mstore_storage_close(st->ram);
    }
    if (st->flash != NULL) {
        mstore_storage_close(st->flash);
    }
    mstore_platform_free(st);
}

static const mstore_storage_ops_t MSTORE_RAM_FLASH_OPS = {
    .read_meta = rf_read_meta,
    .read_key = rf_read_key,
    .read_slot = rf_read_slot,
    .write_slot = rf_write_slot,
    .write_meta = rf_write_meta,
    .clear_all = rf_clear_all,
    .sync = rf_sync,
    .close = rf_close,
};

mstore_err_t mstore_storage_ram_flash_open(const mstore_storage_config_t *config,
                                           mstore_storage_t **out_storage) {
    if (config->backing != (MSTORE_BACKING_RAM | MSTORE_BACKING_FLASH)) {
        return MSTORE_INVALID_ARG;
    }
    if (config->persist_key == NULL || config->persist_key[0] == '\0') {
        return MSTORE_INVALID_ARG;
    }
    if (config->capacity == 0 || config->key_size == 0) {
        return MSTORE_INVALID_SIZE;
    }

    mstore_ram_flash_t *st = mstore_platform_alloc(sizeof(*st));
    if (st == NULL) {
        return MSTORE_NO_MEM;
    }
    st->base.ops = &MSTORE_RAM_FLASH_OPS;
    st->ram = NULL;
    st->flash = NULL;
    st->capacity = config->capacity;
    st->key_size = config->key_size;
    st->payload_size = config->payload_size;

    mstore_storage_config_t flash_config = *config;
    flash_config.backing = MSTORE_BACKING_FLASH;
    mstore_err_t err = mstore_storage_flash_open(&flash_config, &st->flash);
    if (err != MSTORE_OK) {
        rf_close(&st->base);
        return err;
    }

    mstore_storage_config_t ram_config = *config;
    ram_config.backing = MSTORE_BACKING_RAM;
    err = mstore_storage_ram_open(&ram_config, &st->ram);
    if (err != MSTORE_OK) {
        rf_close(&st->base);
        return err;
    }

    /* Seed RAM из durable состояния; generation свободных слотов тоже переносится. */
    void *key_buf = mstore_platform_alloc(st->key_size);
    void *payload_buf = mstore_platform_alloc(st->payload_size == 0 ? 1 : st->payload_size);
    if (key_buf == NULL || payload_buf == NULL) {
        mstore_platform_free(key_buf);
        mstore_platform_free(payload_buf);
        rf_close(&st->base);
        return MSTORE_NO_MEM;
    }

    for (size_t slot = 0; slot < st->capacity && err == MSTORE_OK; slot++) {
        mstore_meta_t meta;
        err = mstore_storage_read_slot(st->flash, (mstore_slot_t)slot, &meta, key_buf, payload_buf);
        if (err == MSTORE_OK) {
            err = mstore_storage_write_slot(st->ram, (mstore_slot_t)slot, &meta, key_buf, payload_buf);
        }
    }
    mstore_platform_free(key_buf);
    mstore_platform_free(payload_buf);

    if (err != MSTORE_OK) {
        rf_close(&st->base);
        return err;
    }

    *out_storage = &st->base;
    return MSTORE_OK;
}
