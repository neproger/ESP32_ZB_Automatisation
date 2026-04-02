#include "gw_model/gw_model_settings.h"

#include <string.h>

#include "micro_db/micro_db_core.h"
#include "gw_model/gw_model_schema.h"

static micro_db_table_t s_settings_table;
static const uint8_t k_settings_key = 1u;

esp_err_t gw_model_init_settings(void)
{
    return micro_db_table_init(&s_settings_table, &GW_MODEL_SCHEMA_SETTINGS);
}

esp_err_t gw_model_deinit_settings(void)
{
    return micro_db_table_deinit(&s_settings_table);
}

esp_err_t gw_model_set_settings(const gw_proto_settings_v1_t *record,
                                bool *out_changed,
                                bool *out_inserted)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }

    bool changed = false;
    bool inserted = false;
    return micro_db_table_upsert(&s_settings_table,
                                 record,
                                 out_changed ? out_changed : &changed,
                                 out_inserted ? out_inserted : &inserted);
}

esp_err_t gw_model_get_settings(gw_proto_settings_v1_t *out_record)
{
    if (!out_record) {
        return ESP_ERR_INVALID_ARG;
    }
    return micro_db_table_get(&s_settings_table, &k_settings_key, out_record);
}
