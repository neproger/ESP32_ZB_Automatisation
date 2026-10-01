#include "storage/mstore_storage.h"

/*
 * Runtime dispatch по backing. Table Engine не ветвится по режиму хранения:
 * backend выбирается один раз при open().
 */
sys_error_t mstore_storage_open(const mstore_storage_config_t *config,
                                 mstore_storage_t **out_storage) {
    if (config == NULL || out_storage == NULL) {
        return mstore_fail(SYS_CODE_INVALID_ARG);
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
    return mstore_fail(SYS_CODE_INVALID_ARG);
}
