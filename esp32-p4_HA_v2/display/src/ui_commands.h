#pragma once

/*
 * Команды, которые UI отправляет в Domain. Реализация — display.c (там живёт domain_t).
 * UI виджетов не знает про domain_t: он зовёт эти функции, как и подобает клиенту.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ha_model/ha_entities.h"
#include "ha_model/ha_properties.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Семантическая команда: target (device/endpoint) + property + action (+ value).
 * ZCL-кодировку делает мост semantics — UI её не знает. */
bool display_send_command(const ha_zb_state_key_t *target, ha_property_id_t property,
                          ha_action_id_t action, const ha_command_value_t *value);

/* Wi-Fi provisioning (docs/services/WEB.md). */
bool display_send_wifi_scan(void);
bool display_send_wifi_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
