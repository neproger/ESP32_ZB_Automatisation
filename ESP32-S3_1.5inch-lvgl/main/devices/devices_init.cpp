#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"

#include "devices_init.h"
#include "ui/ui_app.hpp"
#include "devices/display_init.hpp"
#include "devices/touch_init.hpp"
#include "devices/input_init.hpp"
#include "devices/lvgl_init.hpp"

// Optional debug task can conflict with LVGL touch polling; keep disabled by default
#define ENABLE_TOUCH_DEBUG 0

static const char *TAG = "devices";

/* LVGL display and touch */
static esp_lcd_touch_handle_t touch_handle = NULL;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Touch init wrapper ////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static esp_err_t app_touch_init(void)
{
    return devices_touch_init(&touch_handle);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Devices init /////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
esp_err_t devices_init(void)
{
    esp_err_t err = ESP_OK;

    err = devices_display_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = app_touch_init();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Touch init failed (%s), continue without touch", esp_err_to_name(err));
        touch_handle = NULL;
    }
    else
    {
        esp_log_level_set("CST816S", ESP_LOG_NONE);
    }

    err = devices_lvgl_init(touch_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LVGL init failed: %s", esp_err_to_name(err));
        return err;
    }

    devices_input_init();

    return ESP_OK;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Panel on/off control (public API) /////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

esp_err_t devices_display_set_enabled(bool enabled)
{
    esp_lcd_panel_handle_t panel = devices_display_get_panel();
    if (!panel)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_lcd_panel_disp_on_off(panel, enabled);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD panel disp_on_off(%d) failed: %s", enabled ? 1 : 0, esp_err_to_name(err));
    }
    return err;
}

esp_lcd_touch_handle_t devices_get_touch_handle(void)
{
    return touch_handle;
}

bool devices_has_touch(void)
{
    return touch_handle != NULL;
}




