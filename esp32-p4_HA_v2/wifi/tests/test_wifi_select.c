#include "wifi/wifi_select.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static ha_wifi_scan_record_t scan_of(const char *const ssids[], const int8_t *rssi, uint8_t count)
{
    ha_wifi_scan_record_t scan = {0};
    scan.count = count;
    for (uint8_t i = 0; i < count; ++i) {
        snprintf(scan.aps[i].ssid, sizeof(scan.aps[i].ssid), "%s", ssids[i]);
        scan.aps[i].rssi = rssi[i];
        scan.aps[i].auth = HA_WIFI_AUTH_WPA2;
    }
    return scan;
}

static ha_wifi_known_record_t known_of(const char *ssid)
{
    ha_wifi_known_record_t known = {0};
    snprintf(known.ssid, sizeof(known.ssid), "%s", ssid);
    snprintf(known.password, sizeof(known.password), "secret");
    return known;
}

static void test_picks_strongest_known(void)
{
    const char *const ssids[] = {"Home", "Neighbor", "Cafe"};
    const int8_t rssi[] = {-70, -50, -80};
    ha_wifi_scan_record_t scan = scan_of(ssids, rssi, 3);

    ha_wifi_known_record_t known[2] = {known_of("Home"), known_of("Neighbor")};
    size_t index = 99;
    int8_t best = 0;
    CHECK(wifi_select_known(&scan, known, 2, &index, &best));
    CHECK(index == 1); /* Neighbor -50 сильнее Home -70 */
    CHECK(best == -50);
}

static void test_known_not_in_scan(void)
{
    const char *const ssids[] = {"Cafe"};
    const int8_t rssi[] = {-60};
    ha_wifi_scan_record_t scan = scan_of(ssids, rssi, 1);
    ha_wifi_known_record_t known[1] = {known_of("Home")};
    size_t index = 0;
    int8_t best = 0;
    CHECK(!wifi_select_known(&scan, known, 1, &index, &best));
}

static void test_empty_inputs(void)
{
    const char *const ssids[] = {"Home"};
    const int8_t rssi[] = {-40};
    ha_wifi_scan_record_t scan = scan_of(ssids, rssi, 1);
    ha_wifi_known_record_t known[1] = {known_of("Home")};
    size_t index = 0;
    int8_t best = 0;

    CHECK(!wifi_select_known(&scan, known, 0, &index, &best)); /* нет известных */
    scan.count = 0;
    CHECK(!wifi_select_known(&scan, known, 1, &index, &best)); /* пустой скан */
    CHECK(!wifi_select_known(NULL, known, 1, &index, &best));
}

int main(void)
{
    test_picks_strongest_known();
    test_known_not_in_scan();
    test_empty_inputs();

    if (g_failures != 0) {
        printf("test_wifi_select: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_wifi_select: OK\n");
    return 0;
}
