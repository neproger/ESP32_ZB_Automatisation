#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_core/types.h"

esp_err_t ui_actions_init(void);

esp_err_t ui_actions_enqueue_onoff(const gw_device_uid_t *uid, uint8_t endpoint, bool on);
esp_err_t ui_actions_enqueue_level(const gw_device_uid_t *uid, uint8_t endpoint, uint8_t level);
esp_err_t ui_actions_enqueue_color_temp(const gw_device_uid_t *uid, uint8_t endpoint, uint16_t mireds);

