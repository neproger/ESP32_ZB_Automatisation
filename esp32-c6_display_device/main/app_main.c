#include "esp_log.h"
#include "nvs_flash.h"

#include "rgb_led.h"
#include "display.h"
#include "ui.h"
#include "zb_app.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "ESP32-C6 Zigbee Router starting");

    if (rgb_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed");
    }
    if (display_init() != ESP_OK) {
        ESP_LOGW(TAG, "display init failed");
    }
    ui_init();

    zb_app_start();
}
