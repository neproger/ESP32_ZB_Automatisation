#include "storage/mstore_storage.h"

/*
 * Runtime dispatch по backing. Table Engine не ветвится по режиму хранения:
 * backend выбирается один раз при open().
 * FLASH adapter для esp_partition добавится позже.
 */
mstore_err_t mstore_storage_open(const mstore_storage_config_t *config,
                                 mstore_storage_t **out_storage) {
    if (config == NULL || out_storage == NULL) {
        return MSTORE_INVALID_ARG;
    }
    if (config->backing == MSTORE_BACKING_RAM) {
        return mstore_storage_ram_open(config, out_storage);
    }
    if (config->backing == MSTORE_BACKING_FLASH) {
        return mstore_storage_flash_open(config, out_storage);
    }
    if (config->backing == (MSTORE_BACKING_RAM | MSTORE_BACKING_FLASH)) {
        return mstore_storage_ram_flash_open(config, out_storage);
    }
    return MSTORE_INVALID_ARG;
}
