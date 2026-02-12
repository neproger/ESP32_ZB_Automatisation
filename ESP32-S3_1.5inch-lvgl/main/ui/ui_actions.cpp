#include "ui_actions.hpp"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "gw_zigbee/gw_zigbee.h"

namespace
{
static const char *TAG = "ui_actions";

enum class UiActionType : uint8_t
{
    OnOff = 1,
    Level = 2,
    ColorTemp = 3,
};

typedef struct
{
    UiActionType type;
    gw_device_uid_t uid;
    uint8_t endpoint;
    uint16_t value_u16;
} ui_action_cmd_t;

static QueueHandle_t s_q = nullptr;
static TaskHandle_t s_task = nullptr;

void action_task(void *arg)
{
    (void)arg;
    ui_action_cmd_t cmd = {};
    for (;;)
    {
        if (xQueueReceive(s_q, &cmd, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        esp_err_t err = ESP_OK;
        if (cmd.type == UiActionType::OnOff)
        {
            err = gw_zigbee_onoff_cmd(&cmd.uid, cmd.endpoint, cmd.value_u16 ? GW_ZIGBEE_ONOFF_CMD_ON : GW_ZIGBEE_ONOFF_CMD_OFF);
        }
        else if (cmd.type == UiActionType::Level)
        {
            gw_zigbee_level_t lvl = {.level = (uint8_t)cmd.value_u16, .transition_ms = 200};
            err = gw_zigbee_level_move_to_level(&cmd.uid, cmd.endpoint, lvl);
        }
        else if (cmd.type == UiActionType::ColorTemp)
        {
            gw_zigbee_color_temp_t ct = {.mireds = cmd.value_u16, .transition_ms = 250};
            err = gw_zigbee_color_move_to_temp(&cmd.uid, cmd.endpoint, ct);
        }

        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "action failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t enqueue(const ui_action_cmd_t *cmd)
{
    if (!s_q || !cmd)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_q, cmd, 0) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
} // namespace

esp_err_t ui_actions_init(void)
{
    if (s_q)
    {
        return ESP_OK;
    }
    s_q = xQueueCreate(16, sizeof(ui_action_cmd_t));
    if (!s_q)
    {
        return ESP_ERR_NO_MEM;
    }
#if defined(CONFIG_SPIRAM) || defined(CONFIG_SPIRAM_USE)
    if (xTaskCreateWithCaps(action_task, "ui_actions", 3072, nullptr, 4, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
#else
    if (xTaskCreate(action_task, "ui_actions", 3072, nullptr, 4, &s_task) != pdPASS)
#endif
    {
        vQueueDelete(s_q);
        s_q = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ui_actions_enqueue_onoff(const gw_device_uid_t *uid, uint8_t endpoint, bool on)
{
    if (!uid || !uid->uid[0])
    {
        return ESP_ERR_INVALID_ARG;
    }
    ui_action_cmd_t cmd = {};
    cmd.type = UiActionType::OnOff;
    cmd.uid = *uid;
    cmd.endpoint = endpoint;
    cmd.value_u16 = on ? 1 : 0;
    return enqueue(&cmd);
}

esp_err_t ui_actions_enqueue_level(const gw_device_uid_t *uid, uint8_t endpoint, uint8_t level)
{
    if (!uid || !uid->uid[0])
    {
        return ESP_ERR_INVALID_ARG;
    }
    ui_action_cmd_t cmd = {};
    cmd.type = UiActionType::Level;
    cmd.uid = *uid;
    cmd.endpoint = endpoint;
    cmd.value_u16 = level;
    return enqueue(&cmd);
}

esp_err_t ui_actions_enqueue_color_temp(const gw_device_uid_t *uid, uint8_t endpoint, uint16_t mireds)
{
    if (!uid || !uid->uid[0])
    {
        return ESP_ERR_INVALID_ARG;
    }
    ui_action_cmd_t cmd = {};
    cmd.type = UiActionType::ColorTemp;
    cmd.uid = *uid;
    cmd.endpoint = endpoint;
    cmd.value_u16 = mireds;
    return enqueue(&cmd);
}
