# esp32-c6_display_device

Автономный узел Zigbee на плате **Waveshare ESP32-C6-LCD-1.47** (ST7789 172x320,
SPI2). Питается от сети и работает в роли **Zigbee Router**: постоянно включён,
ретранслирует трафик и может быть parent'ом до 10 спящих/конечных устройств.

## Возможности

- **LCD-дашборд** (LVGL + `esp_lvgl_port`): состояние сети, PAN/канал/адреса,
  состояние лампы, счётчик нажатий и лог RX/TX.
- **ZCL Color Dimmable Light** (endpoint 2, server) на бортовом WS2812:
  On/Off, Level, Color Control (Hue/Saturation и CIE xy).
- **ZCL On/Off Switch** (endpoint 1, client): кнопка BOOT шлёт стандартную
  команду On/Off Toggle координатору.
- **Комиссионирование**: network steering с ретраями; long-press (>5 с) —
  factory reset.

## Подключение (board.h)

| Сигнал | GPIO |
| --- | --- |
| LCD SCLK / MOSI | 7 / 6 |
| LCD CS / DC / RST / BL | 14 / 15 / 21 / 22 |
| RGB LED (WS2812) | 8 |
| Кнопка BOOT (active-low) | 9 |

## Модель

- Manufacturer `ESPRESSIF`, Model `ESP32C6-DISPLAY` (см. `zb_app.c`).
- Партиции: `nvs` (datasets Zigbee), `factory`, `zb_fct` — `partitions.csv`.

## Сборка

```sh
idf.py set-target esp32c6
idf.py build flash monitor
```

Зависимости: ESP-IDF >= 6.0, `espressif/esp-zigbee-lib` 2.x, `lvgl/lvgl` 9.x,
`espressif/esp_lvgl_port`, `espressif/led_strip`.
