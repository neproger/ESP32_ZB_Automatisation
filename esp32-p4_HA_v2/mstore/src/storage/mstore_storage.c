#include "storage/mstore_storage.h"

/*
 * Runtime dispatch по backing. Table Engine не ветвится по режиму хранения:
 * backend выбирается один раз при open().
 * FLASH / RAM|FLASH будут добавлены вместе со своими backend'ами.
 */
mstore_err_t mstore_storage_open(const mstore_storage_config_t *config,
                                 mstore_storage_t **out_storage) {
    if (config == NULL || out_storage == NULL) {
        return MSTORE_INVALID_ARG;
    }
    switch (config->backing) {
        case MSTORE_BACKING_RAM:
            return mstore_storage_ram_open(config, out_storage);
        default:
            return MSTORE_INVALID_ARG;
    }
}
