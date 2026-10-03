#pragma once

/* Zigbee Router (mains-powered, always-on mesh node): standard HA On/Off
 * switch (client) + Color Dimmable Light (server, on-board RGB LED). The
 * button on the switch endpoint sends a standard On/Off Toggle command to the
 * coordinator. Running as a router, it also relays traffic and parents up to
 * 10 sleepy/end devices (see zb_app.c). */

void zb_app_start(void);
