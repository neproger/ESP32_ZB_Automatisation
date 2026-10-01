#include "button.h"
#include "board.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

#define BUTTON_POLL_MS      20
#define BUTTON_LONG_PRESS_MS 5000

static button_event_cb_t s_cb;
static void *s_ctx;

static void button_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    bool long_fired = false;
    int64_t press_start_us = 0;

    for (;;) {
        const bool down = (gpio_get_level(BOARD_BUTTON_GPIO) == BOARD_BUTTON_ACTIVE_LEVEL);

        if (down && !pressed) {
            pressed = true;
            long_fired = false;
            press_start_us = esp_timer_get_time();
        } else if (down && pressed && !long_fired) {
            const int64_t held_ms = (esp_timer_get_time() - press_start_us) / 1000;
            if (held_ms >= BUTTON_LONG_PRESS_MS) {
                long_fired = true;
                if (s_cb) {
                    s_cb(BUTTON_EVENT_LONG_PRESS, s_ctx);
                }
            }
        } else if (!down && pressed) {
            pressed = false;
            if (!long_fired && s_cb) {
                s_cb(BUTTON_EVENT_SHORT_PRESS, s_ctx);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_init(button_event_cb_t cb, void *user_ctx)
{
    s_cb = cb;
    s_ctx = user_ctx;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config failed");

    if (xTaskCreate(button_task, "button", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create button task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
