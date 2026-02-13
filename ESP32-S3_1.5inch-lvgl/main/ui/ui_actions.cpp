#include "ui_actions.hpp"

#include "gw_zigbee/gw_zigbee.h"

esp_err_t ui_actions_init(void)
{
    return ESP_OK;
}

esp_err_t ui_actions_enqueue_onoff(const gw_device_uid_t *uid, uint8_t endpoint, bool on)
{
    if (!uid || !uid->uid[0] || endpoint == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return gw_zigbee_onoff_cmd(uid, endpoint, on ? GW_ZIGBEE_ONOFF_CMD_ON : GW_ZIGBEE_ONOFF_CMD_OFF);
}

esp_err_t ui_actions_enqueue_level(const gw_device_uid_t *uid, uint8_t endpoint, uint8_t level)
{
    if (!uid || !uid->uid[0] || endpoint == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    gw_zigbee_level_t cmd = {
        .level = level,
        .transition_ms = 0,
    };
    return gw_zigbee_level_move_to_level(uid, endpoint, cmd);
}

esp_err_t ui_actions_enqueue_color_xy(const gw_device_uid_t *uid, uint8_t endpoint, uint16_t x, uint16_t y)
{
    if (!uid || !uid->uid[0] || endpoint == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    gw_zigbee_color_xy_t cmd = {
        .x = x,
        .y = y,
        .transition_ms = 0,
    };
    return gw_zigbee_color_move_to_xy(uid, endpoint, cmd);
}

esp_err_t ui_actions_enqueue_color_temp(const gw_device_uid_t *uid, uint8_t endpoint, uint16_t mireds)
{
    if (!uid || !uid->uid[0] || endpoint == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    gw_zigbee_color_temp_t cmd = {
        .mireds = mireds,
        .transition_ms = 0,
    };
    return gw_zigbee_color_move_to_temp(uid, endpoint, cmd);
}
