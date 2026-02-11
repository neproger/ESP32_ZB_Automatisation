#pragma once

/*
 * Copy this file to `main/wifi_aps_config.h` (not committed; ignored by .gitignore)
 * and fill in your Wi‑Fi credentials.
 *
 * The firmware will try APs in order until it gets an IP address.
 */

#include <stddef.h>

typedef struct
{
    const char *ssid;
    const char *password;
} gw_wifi_ap_credential_t;

static const gw_wifi_ap_credential_t GW_WIFI_APS[] = {
    {.ssid = "ITPod", .password = "zxcxzzxc"},
    {.ssid = "oneplus8", .password = "zxcxzzxc"}, //MERCUSYS_0348
    {.ssid = "MERCUSYS_0348", .password = "18188530zxc"},
};

static const size_t GW_WIFI_APS_COUNT = sizeof(GW_WIFI_APS) / sizeof(GW_WIFI_APS[0]);

