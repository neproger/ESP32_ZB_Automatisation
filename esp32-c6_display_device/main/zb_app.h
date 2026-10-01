#pragma once

/* Zigbee End Device: standard HA On/Off switch (client) + Color Dimmable
 * Light (server, on-board RGB LED). The button on the switch endpoint sends
 * a standard On/Off Toggle command to the coordinator. */

void zb_app_start(void);
