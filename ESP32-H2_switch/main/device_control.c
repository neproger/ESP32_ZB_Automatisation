#include "device_control.h"

#include "esp_log.h"

static const char *TAG = "DEVICE_CTRL";

/* Safety thresholds in 0.01 °C. */
#define DEVICE_OVER_TEMP_CENTI  (8000) /* > 80.00 °C -> cut */
#define DEVICE_RESUME_TEMP_CENTI (7000) /* <= 70.00 °C -> allow again */

/* Consecutive failed reads before the sensor is considered lost. */
#define DEVICE_TEMP_LOST_THRESHOLD (2)

static device_relay_cb_t s_relay_cb;
static bool s_device_on;
static bool s_relay_on;
static bool s_temp_valid;
static bool s_over_temp;
static uint8_t s_temp_fail_count;

static void device_apply(void)
{
    const bool relay_on = s_device_on && s_temp_valid && !s_over_temp;
    if (relay_on == s_relay_on) {
        return;
    }
    s_relay_on = relay_on;
    ESP_LOGI(TAG, "Relay -> %s (power=%d temp_valid=%d over_temp=%d)", relay_on ? "ON" : "OFF", s_device_on,
             s_temp_valid, s_over_temp);
    if (s_relay_cb) {
        s_relay_cb(relay_on);
    }
}

void device_control_init(device_relay_cb_t relay_cb)
{
    s_relay_cb = relay_cb;
    s_device_on = false; /* reboot => OFF */
    s_relay_on = false;
    s_temp_valid = false;
    s_over_temp = false;
    s_temp_fail_count = 0;
    if (s_relay_cb) {
        s_relay_cb(false); /* ensure hardware starts OFF */
    }
}

void device_control_set_power(bool on)
{
    if (s_device_on == on) {
        return;
    }
    s_device_on = on;
    ESP_LOGI(TAG, "Device power -> %s", on ? "ON" : "OFF");
    device_apply();
}

void device_control_notify_temperature(int16_t temp_centi_c)
{
    s_temp_fail_count = 0;
    s_temp_valid = true;

    if (temp_centi_c > DEVICE_OVER_TEMP_CENTI) {
        if (!s_over_temp) {
            ESP_LOGW(TAG, "Over-temp %.2f C > 80 C, cutting relay", temp_centi_c / 100.0f);
        }
        s_over_temp = true;
    } else if (s_over_temp && temp_centi_c <= DEVICE_RESUME_TEMP_CENTI) {
        ESP_LOGI(TAG, "Cooled to %.2f C <= 70 C, relay allowed", temp_centi_c / 100.0f);
        s_over_temp = false;
    }

    device_apply();
}

void device_control_notify_temperature_lost(void)
{
    if (s_temp_fail_count < 255) {
        s_temp_fail_count++;
    }
    if (s_temp_fail_count >= DEVICE_TEMP_LOST_THRESHOLD) {
        if (s_temp_valid) {
            ESP_LOGW(TAG, "DS18B20 lost, cutting relay");
        }
        s_temp_valid = false;
    }
    device_apply();
}

bool device_control_get_power(void)
{
    return s_device_on;
}

bool device_control_get_relay_state(void)
{
    return s_relay_on;
}
