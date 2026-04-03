#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_proto/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fast-path executor for compiled rules (avoids dynamic allocations at runtime).
// Currently supports the MVP zigbee commands used by Automations UI:
// - onoff.on / onoff.off / onoff.toggle
// - level.move_to_level (arg0_u32=level 0..254, arg1_u32=transition_ms)
esp_err_t gw_action_exec_compiled_zigbee(const char *cmd,
                                        const gw_device_uid_t *device_uid,
                                        uint8_t endpoint,
                                        uint32_t arg0_u32,
                                        uint32_t arg1_u32,
                                        uint32_t arg2_u32,
                                        char *err,
                                        size_t err_size);

esp_err_t gw_action_exec_action(const gw_automation_entry_t *entry,
                                const gw_auto_bin_action_v2_t *action,
                                char *err,
                                size_t err_size);

esp_err_t gw_action_exec_entry(const gw_automation_entry_t *entry,
                               char *err,
                               size_t err_size);

#ifdef __cplusplus
}
#endif
