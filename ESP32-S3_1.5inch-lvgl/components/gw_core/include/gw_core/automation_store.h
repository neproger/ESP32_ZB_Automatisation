// automation_store.h - Now using universal storage backend
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "gw_core/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*gw_automation_store_listener_t)(void *user_ctx);

esp_err_t gw_automation_store_init(void);
esp_err_t gw_automation_store_add_listener(gw_automation_store_listener_t cb, void *user_ctx);
esp_err_t gw_automation_store_remove_listener(gw_automation_store_listener_t cb, void *user_ctx);
size_t gw_automation_store_count(void);
size_t gw_automation_store_list(gw_automation_entry_t *out, size_t max_out);
size_t gw_automation_store_list_meta(gw_automation_meta_t *out, size_t max_out);
esp_err_t gw_automation_store_get(const char *id, gw_automation_entry_t *out);
esp_err_t gw_automation_store_get_by_index(size_t index, gw_automation_entry_t *out);
esp_err_t gw_automation_store_put_entry(const gw_automation_entry_t *entry);
esp_err_t gw_automation_store_remove(const char *id);
esp_err_t gw_automation_store_set_enabled(const char *id, bool enabled);
esp_err_t gw_automation_store_remove_all(void);

#ifdef __cplusplus
}
#endif
