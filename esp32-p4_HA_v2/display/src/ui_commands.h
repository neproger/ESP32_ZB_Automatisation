#pragma once

/*
 * Команды, которые UI отправляет в Domain. Реализация — display.c (там живёт domain_t).
 * UI виджетов не знает про domain_t: он зовёт эти функции, как и подобает клиенту.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ha_model/ha_entity.h"
#include "ha_model/ha_properties.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Семантическая команда по entity (Шаг A): entity + property + action (+ value).
 * Кодировку и адресацию делает adapter — UI транспорта не знает. */
bool display_send_command(ha_entity_id_t entity, ha_property_id_t property, ha_action_id_t action,
                          const ha_command_value_t *value);

/* Плитка автоматизации: записать правило с новым enabled (read-modify-write из UI). */
bool display_set_automation_enabled(uint64_t automation, bool enabled);

/* Wi-Fi provisioning (docs/services/WEB.md). */
bool display_send_wifi_scan(void);
bool display_send_wifi_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
