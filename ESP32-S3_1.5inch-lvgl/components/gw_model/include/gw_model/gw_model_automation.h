#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "gw_proto/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_model_init_automation(void);
esp_err_t gw_model_deinit_automation(void);

esp_err_t gw_model_upsert_automation(const gw_automation_entry_t *record,
                                     bool *out_changed,
                                     bool *out_inserted);
esp_err_t gw_model_get_automation(const char *id,
                                  gw_automation_entry_t *out_record);
esp_err_t gw_model_remove_automation(const char *id,
                                     bool *out_removed);
esp_err_t gw_model_clear_automations(void);
size_t gw_model_count_automations(void);
size_t gw_model_list_automations(gw_automation_entry_t *out, size_t max_out);
esp_err_t gw_model_get_automation_by_index(size_t index,
                                           gw_automation_entry_t *out_record);

#ifdef __cplusplus
}
#endif
