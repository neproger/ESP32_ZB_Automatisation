#pragma once

/*
 * Порт Display под плату Guition JC4880P443C_I_W (ESP32-P4):
 * ST7701 480x800 MIPI-DSI + GT911 touch + esp_lvgl_port.
 *
 * Поднимает панель и тач, заводит default LVGL display, затем вызывает
 * display_start(domain) (компонент `display`) под LVGL-мьютексом.
 */

#include "domain/domain.h"

#ifdef __cplusplus
extern "C" {
#endif

void display_p4_start(domain_t *domain);

#ifdef __cplusplus
}
#endif
