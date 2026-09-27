#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

/*
 * Sensors:
 *  - DHT11  : single-wire, needs external 10k pull-up to 3V3
 *  - DS18B20: 1-Wire,     needs external 4.7k pull-up to 3V3
 *
 * ESP32-H2 SuperMini pins already used: GP8 (RGB), GP9 (BOOT), GP12 (relay).
 */
#define SENSOR_DHT11_GPIO    GPIO_NUM_11
#define SENSOR_DS18B20_GPIO  GPIO_NUM_10

esp_err_t sensors_init(void);

/* DHT11 read. On success fills ZCL units: temperature in 0.01 °C, humidity in 0.01 %. */
esp_err_t sensors_read_dht11(int16_t *temp_centi_c, uint16_t *humi_centi_pct);

/* DS18B20 read. On success fills ZCL units: temperature in 0.01 °C. */
esp_err_t sensors_read_ds18b20(int16_t *temp_centi_c);
