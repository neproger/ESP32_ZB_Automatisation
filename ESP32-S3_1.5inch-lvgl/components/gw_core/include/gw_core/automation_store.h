// automation_store.h - Now using universal storage backend
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_automation_store_init(void);
size_t gw_automation_store_list(gw_automation_entry_t *out, size_t max_out);
size_t gw_automation_store_list_meta(gw_automation_meta_t *out, size_t max_out);
esp_err_t gw_automation_store_get(const char *id, gw_automation_entry_t *out);
esp_err_t gw_automation_store_put_cbor(const uint8_t *buf, size_t len);
esp_err_t gw_automation_store_remove(const char *id);
esp_err_t gw_automation_store_set_enabled(const char *id, bool enabled);

#ifdef __cplusplus
}
#endif
