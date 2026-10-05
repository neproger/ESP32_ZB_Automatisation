#pragma once

/*
 * Чистая логика Wi-Fi provisioning (без ESP-IDF): выбор известной точки с самым
 * сильным сигналом среди результатов скана — для автоподключения при старте
 * (docs/services/WIFI.md). Тестируется на host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ha_model/ha_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ищет среди `scan` точку, которая есть в списке известных `known[known_count]`, и
 * возвращает индекс известной с максимальным rssi. false — ни одной известной нет.
 */
bool wifi_select_known(const ha_wifi_scan_record_t *scan,
                       const ha_wifi_known_record_t *known, size_t known_count,
                       size_t *out_index, int8_t *out_rssi);

#ifdef __cplusplus
}
#endif
