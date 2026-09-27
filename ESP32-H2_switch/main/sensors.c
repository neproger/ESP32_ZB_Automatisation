#include "sensors.h"

#include <math.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SENSORS";

/* ------------------------------------------------------------------ */
/* GPIO setup (open-drain so the external pull-up owns the idle high)  */
/* ------------------------------------------------------------------ */

static esp_err_t sensor_gpio_init(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        gpio_set_level(pin, 1); /* released / idle high */
    }
    return err;
}

esp_err_t sensors_init(void)
{
    ESP_RETURN_ON_ERROR(sensor_gpio_init(SENSOR_DHT11_GPIO), TAG, "DHT11 GPIO init failed");
    ESP_RETURN_ON_ERROR(sensor_gpio_init(SENSOR_DS18B20_GPIO), TAG, "DS18B20 GPIO init failed");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* DHT11 (single-wire, timing critical)                                */
/* ------------------------------------------------------------------ */

static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t sensors_read_dht11(int16_t *temp_centi_c, uint16_t *humi_centi_pct)
{
    if (!temp_centi_c || !humi_centi_pct) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[5] = {0};

    /* Start signal: hold the line low for > 18 ms. */
    gpio_set_level(SENSOR_DHT11_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(SENSOR_DHT11_GPIO, 1);
    esp_rom_delay_us(30);

    portENTER_CRITICAL(&s_dht_mux);

    int t = 0;
    while (gpio_get_level(SENSOR_DHT11_GPIO) == 1) {
        if (++t > 100) {
            portEXIT_CRITICAL(&s_dht_mux);
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }
    t = 0;
    while (gpio_get_level(SENSOR_DHT11_GPIO) == 0) {
        if (++t > 100) {
            portEXIT_CRITICAL(&s_dht_mux);
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }
    t = 0;
    while (gpio_get_level(SENSOR_DHT11_GPIO) == 1) {
        if (++t > 100) {
            portEXIT_CRITICAL(&s_dht_mux);
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }

    for (int i = 0; i < 40; ++i) {
        while (gpio_get_level(SENSOR_DHT11_GPIO) == 0) {
        }
        esp_rom_delay_us(40);
        if (gpio_get_level(SENSOR_DHT11_GPIO) == 1) {
            data[i >> 3] |= (uint8_t)(1u << (7 - (i & 7)));
        }
        while (gpio_get_level(SENSOR_DHT11_GPIO) == 1) {
        }
    }

    portEXIT_CRITICAL(&s_dht_mux);

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    *humi_centi_pct = (uint16_t)data[0] * 100u;
    *temp_centi_c = (int16_t)data[2] * 100;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* DS18B20 (1-Wire, timing critical)                                   */
/* ------------------------------------------------------------------ */

#define DS_CMD_SKIP_ROM      0xCC
#define DS_CMD_CONVERT_T     0x44
#define DS_CMD_READ_SCRATCH  0xBE

static portMUX_TYPE s_ds_mux = portMUX_INITIALIZER_UNLOCKED;

static bool ds_ow_reset(void)
{
    portENTER_CRITICAL(&s_ds_mux);
    gpio_set_level(SENSOR_DS18B20_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(SENSOR_DS18B20_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(SENSOR_DS18B20_GPIO) == 0);
    esp_rom_delay_us(410);
    portEXIT_CRITICAL(&s_ds_mux);
    return present;
}

static void ds_ow_write_byte(uint8_t byte)
{
    portENTER_CRITICAL(&s_ds_mux);
    for (int i = 0; i < 8; ++i) {
        if (byte & (1u << i)) {
            gpio_set_level(SENSOR_DS18B20_GPIO, 0);
            esp_rom_delay_us(3);
            gpio_set_level(SENSOR_DS18B20_GPIO, 1);
            esp_rom_delay_us(62);
        } else {
            gpio_set_level(SENSOR_DS18B20_GPIO, 0);
            esp_rom_delay_us(62);
            gpio_set_level(SENSOR_DS18B20_GPIO, 1);
            esp_rom_delay_us(3);
        }
    }
    portEXIT_CRITICAL(&s_ds_mux);
}

static uint8_t ds_ow_read_byte(void)
{
    uint8_t byte = 0;
    portENTER_CRITICAL(&s_ds_mux);
    for (int i = 0; i < 8; ++i) {
        gpio_set_level(SENSOR_DS18B20_GPIO, 0);
        esp_rom_delay_us(3);
        gpio_set_level(SENSOR_DS18B20_GPIO, 1);
        esp_rom_delay_us(10);
        if (gpio_get_level(SENSOR_DS18B20_GPIO)) {
            byte |= (uint8_t)(1u << i);
        }
        esp_rom_delay_us(53);
    }
    portEXIT_CRITICAL(&s_ds_mux);
    return byte;
}

esp_err_t sensors_read_ds18b20(int16_t *temp_centi_c)
{
    if (!temp_centi_c) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ds_ow_reset()) {
        return ESP_ERR_NOT_FOUND;
    }
    ds_ow_write_byte(DS_CMD_SKIP_ROM);
    ds_ow_write_byte(DS_CMD_CONVERT_T);

    /* 12-bit conversion takes up to 750 ms. */
    vTaskDelay(pdMS_TO_TICKS(750));

    if (!ds_ow_reset()) {
        return ESP_ERR_NOT_FOUND;
    }
    ds_ow_write_byte(DS_CMD_SKIP_ROM);
    ds_ow_write_byte(DS_CMD_READ_SCRATCH);

    uint8_t sp[9];
    for (int i = 0; i < 9; ++i) {
        sp[i] = ds_ow_read_byte();
    }

    int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | sp[0]);
    float celsius = (float)raw * 0.0625f;
    *temp_centi_c = (int16_t)lroundf(celsius * 100.0f);
    return ESP_OK;
}
