#include "wifi/wifi_select.h"

#include <string.h>

bool wifi_select_known(const ha_wifi_scan_record_t *scan,
                       const ha_wifi_known_record_t *known, size_t known_count,
                       size_t *out_index, int8_t *out_rssi)
{
    if (scan == NULL || known == NULL || out_index == NULL || out_rssi == NULL) {
        return false;
    }
    if (known_count == 0 || scan->count == 0) {
        return false;
    }

    const uint8_t count = (scan->count > HA_WIFI_SCAN_MAX) ? HA_WIFI_SCAN_MAX : scan->count;
    bool found = false;
    int8_t best_rssi = 0;
    size_t best_index = 0;

    for (uint8_t i = 0; i < count; ++i) {
        for (size_t k = 0; k < known_count; ++k) {
            if (strncmp(scan->aps[i].ssid, known[k].ssid, HA_WIFI_SSID_MAX) != 0) {
                continue;
            }
            if (!found || scan->aps[i].rssi > best_rssi) {
                found = true;
                best_rssi = scan->aps[i].rssi;
                best_index = k;
            }
            break;
        }
    }

    if (!found) {
        return false;
    }
    *out_index = best_index;
    *out_rssi = best_rssi;
    return true;
}
