#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Dashboard shown on the on-board 172x320 LCD. */

void ui_init(void);

/* Network state, e.g. "JOINING", "JOINED", "OFFLINE". */
void ui_set_network_state(const char *state, uint32_t rgb);

/* Network information line. */
void ui_set_net_info(uint16_t pan_id, uint8_t channel, uint16_t short_addr, const char *ieee_str);

/* Lamp state as reported by the light endpoint. */
void ui_set_light(bool on, uint8_t level, uint8_t hue, uint8_t sat);

/* On-chip die temperature in degrees Celsius. */
void ui_set_temperature(float celsius);

/* Number of local button presses. */
void ui_set_button_count(uint32_t count);

/* Append a line to the RX/TX log. */
void ui_add_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
