#pragma once

/*
 * Команды, которые UI отправляет в Domain. Реализация — display.c (там живёт domain_t).
 * UI виджетов не знает про domain_t: он зовёт эти функции, как и подобает клиенту.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ha_model/ha_entities.h"

#ifdef __cplusplus
extern "C" {
#endif

bool display_send_onoff(const ha_zb_state_key_t *key, bool on);
bool display_send_level(const ha_zb_state_key_t *key, uint8_t level);
bool display_send_hue(const ha_zb_state_key_t *key, uint8_t hue);
bool display_send_saturation(const ha_zb_state_key_t *key, uint8_t saturation);
bool display_send_color_temperature(const ha_zb_state_key_t *key, uint16_t mireds);

/* Wi-Fi provisioning (docs/services/WEB.md). */
bool display_send_wifi_scan(void);
bool display_send_wifi_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
