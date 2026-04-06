#include "gw_model/gw_model_automation.h"

#include <string.h>

#include "micro_db/micro_db_core.h"
#include "gw_model_notify.h"
#include "gw_model/gw_model_schema.h"
#include "gw_proto/gw_proto_validate.h"

static micro_db_table_t s_automation_table;

typedef struct {
    char id[GW_AUTOMATION_ID_MAX];
} gw_model_automation_key_t;

static void fill_automation_key(const char *id, gw_model_automation_key_t *out_key)
{
    memset(out_key, 0, sizeof(*out_key));
    if (id) {
        strlcpy(out_key->id, id, sizeof(out_key->id));
    }
}

esp_err_t gw_model_init_automation(void)
{
    return micro_db_table_init(&s_automation_table, &GW_MODEL_SCHEMA_AUTOMATION);
}

esp_err_t gw_model_deinit_automation(void)
{
    return micro_db_table_deinit(&s_automation_table);
}

esp_err_t gw_model_upsert_automation(const gw_automation_entry_t *record,
                                     bool *out_changed,
                                     bool *out_inserted)
{
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_automation_entry_t trimmed = {0};
    gw_proto_trim_automation_entry(&trimmed, record);
    record = &trimmed;

    bool changed = false;
    bool inserted = false;
    esp_err_t err = micro_db_table_upsert(&s_automation_table,
                                          record,
                                          out_changed ? out_changed : &changed,
                                          out_inserted ? out_inserted : &inserted);
    if (err == ESP_OK && (changed || inserted)) {
        (void)gw_model_notify_automation_upsert(record);
    }
    return err;
}

esp_err_t gw_model_get_automation(const char *id,
                                  gw_automation_entry_t *out_record)
{
    if (!out_record || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_model_automation_key_t key = {0};
    fill_automation_key(id, &key);
    return micro_db_table_get(&s_automation_table, &key, out_record);
}

esp_err_t gw_model_remove_automation(const char *id,
                                     bool *out_removed)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_model_automation_key_t key = {0};
    fill_automation_key(id, &key);
    bool removed = false;
    esp_err_t err = micro_db_table_remove(&s_automation_table, &key, out_removed ? out_removed : &removed);
    if (err == ESP_OK && removed) {
        (void)gw_model_notify_automation_remove(id);
    }
    return err;
}

size_t gw_model_count_automations(void)
{
    return micro_db_table_count(&s_automation_table);
}

size_t gw_model_list_automations(gw_automation_entry_t *out, size_t max_out)
{
    if (!out || max_out == 0) {
        return 0;
    }

    const size_t count = micro_db_table_count(&s_automation_table);
    const size_t limit = count < max_out ? count : max_out;
    size_t copied = 0;
    for (size_t i = 0; i < limit; ++i) {
        if (micro_db_table_get_by_index(&s_automation_table, i, &out[copied]) == ESP_OK) {
            copied++;
        }
    }
    return copied;
}

esp_err_t gw_model_get_automation_by_index(size_t index,
                                           gw_automation_entry_t *out_record)
{
    return micro_db_table_get_by_index(&s_automation_table, index, out_record);
}
