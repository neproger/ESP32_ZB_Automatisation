#pragma once

#include "esp_err.h"
#include "gw_core/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_uart_link_start(void);
void gw_uart_link_set_snapshot_ready(bool ready);
esp_err_t gw_uart_link_send_event_zb(const gw_proto_event_v1_t *event);
esp_err_t gw_uart_link_request_snapshot_debounced(void);

#ifdef __cplusplus
}
#endif
