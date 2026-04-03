#pragma once

#include "esp_err.h"

#include "micro_db/micro_db_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t micro_db_flash_init(void);

esp_err_t micro_db_flash_load_table(micro_db_table_t *table);
esp_err_t micro_db_flash_persist_table(const micro_db_table_t *table);
esp_err_t micro_db_flash_write_slot(const micro_db_table_t *table, uint32_t slot);
esp_err_t micro_db_flash_write_slot_used(const micro_db_table_t *table, uint32_t slot);
esp_err_t micro_db_flash_write_meta(const micro_db_table_t *table);
esp_err_t micro_db_flash_clear_table(const micro_db_table_t *table);

#ifdef __cplusplus
}
#endif
