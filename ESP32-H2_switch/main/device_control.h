#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Device state machine.
 *
 * Desired state (device power, set from HA):
 *   - reboot => OFF (not persisted)
 *
 * Actual relay state:
 *   relay_on = device_on && temp_valid && !over_temp
 *
 * DS18B20 safety (hysteresis):
 *   - temp > 80.00 C  => over_temp latched, relay cut
 *   - temp <= 70.00 C => over_temp cleared, relay allowed again
 *   - sensor lost (N consecutive failures) => temp invalid, relay cut
 */

typedef void (*device_relay_cb_t)(bool relay_on);

void device_control_init(device_relay_cb_t relay_cb);

/* Called from the Zigbee action handler (stack context, ZB lock held). */
void device_control_set_power(bool on);
bool device_control_get_power(void);

/* Called from the sensor polling task (no ZB lock held). */
void device_control_notify_temperature(int16_t temp_centi_c);
void device_control_notify_temperature_lost(void);

bool device_control_get_relay_state(void);
