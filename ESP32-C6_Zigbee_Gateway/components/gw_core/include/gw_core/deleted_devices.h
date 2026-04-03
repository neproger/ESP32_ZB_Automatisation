#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gw_device_uid_t device_uid;
    uint64_t removed_at_ms;
} gw_deleted_device_t;

esp_err_t gw_deleted_devices_init(void);
esp_err_t gw_deleted_devices_add(const gw_device_uid_t *uid, uint64_t removed_at_ms);
esp_err_t gw_deleted_devices_remove(const gw_device_uid_t *uid);
bool gw_deleted_devices_contains(const gw_device_uid_t *uid);
size_t gw_deleted_devices_count(void);

#ifdef __cplusplus
}
#endif
