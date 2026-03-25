#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ntp_server;
    uint32_t sync_interval_ms;
    uint32_t sync_timeout_ms;
} gw_net_time_cfg_t;

esp_err_t gw_net_time_init(const gw_net_time_cfg_t *cfg);
esp_err_t gw_net_time_deinit(void);

bool gw_net_time_is_synced(void);
uint64_t gw_net_time_now_ms(void);
uint64_t gw_net_time_last_sync_ms(void);

#ifdef __cplusplus
}
#endif
