#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_core/types.h"

typedef enum
{
    UI_CONTROL_ACK_IDLE = 0,
    UI_CONTROL_ACK_PENDING = 1,
    UI_CONTROL_ACK_ERROR = 2,
} ui_control_ack_status_t;

bool ui_control_ack_begin(const char *device_uid, uint8_t endpoint, const char *key);
void ui_control_ack_fail(const char *device_uid, uint8_t endpoint, const char *key);
bool ui_control_ack_is_pending(const char *device_uid, uint8_t endpoint, const char *key);
ui_control_ack_status_t ui_control_ack_get_status(const char *device_uid, uint8_t endpoint, const char *key);
void ui_control_ack_poll_timeouts(uint64_t now_ms, uint32_t timeout_ms);

void ui_control_ack_confirm_bool(const char *device_uid, uint8_t endpoint, const char *key, bool has_value, bool value);
bool ui_control_ack_get_confirmed_bool(const char *device_uid, uint8_t endpoint, const char *key, bool *out_has_value, bool *out_value);
