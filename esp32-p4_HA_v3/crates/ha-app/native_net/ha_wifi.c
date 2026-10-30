/*
 * Wi-Fi service on ESP-Hosted (P4 host, C3 slave).
 *
 * One owner task: known-AP list, autoconnect to the strongest known AP,
 * exponential backoff and a link watchdog. Radio calls never run in an event
 * or timer callback, so a stuck connect cannot block the reconnect path.
 *
 * Provisioning is done from the display; known APs are kept in NVS.
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ha_net";

#define WIFI_TASK_STACK 8192
#define WIFI_TASK_PRIORITY 4
#define WIFI_WATCHDOG_MS 5000
#define WIFI_RECONNECT_GRACE_MS 15000
#define RECONNECT_MAX_SHIFT 5
#define WIFI_KNOWN_MAX 8
#define WIFI_SCAN_MAX 16
#define WIFI_SSID_MAX 33
#define WIFI_PASSWORD_MAX 65
/* 0.25 dBm units. C3 CP on IDF 6.1 gets AUTH_EXPIRE at full power; see docs/WIFI.md. */
#define WIFI_MAX_TX_POWER 34

typedef struct {
    char ssid[WIFI_SSID_MAX];
    char password[WIFI_PASSWORD_MAX];
} wifi_known_t;

typedef struct {
    char ssid[WIFI_SSID_MAX];
    int8_t rssi;
    uint8_t bssid[6];
    uint8_t channel;
} wifi_scan_ap_t;

typedef enum {
    WIFI_REQ_RECONNECT = 0,
    WIFI_REQ_SCAN = 1,
    WIFI_REQ_CONNECT = 2,
} wifi_req_kind_t;

typedef struct {
    uint8_t kind;
    char ssid[WIFI_SSID_MAX];
    char password[WIFI_PASSWORD_MAX];
} wifi_req_t;

static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_scan_mutex;
static wifi_scan_ap_t s_scan_results[WIFI_SCAN_MAX];
static size_t s_scan_count;
static bool s_scan_ready;
static wifi_known_t s_known[WIFI_KNOWN_MAX];
static size_t s_known_count;
/* Link is up (GOT_IP): the watchdog distinguishes "connecting" from "no link". */
static volatile bool s_connected;
/* Reconnect already requested: do not queue a duplicate per DISCONNECTED. */
static volatile bool s_reconnect_pending;
/* Consecutive failed attempts; drives the 1..16 s backoff. */
static uint32_t s_fail_count;
/* Last connect request: the watchdog gives DHCP time before interfering. */
static TickType_t s_last_connect_at;

static void store_scan_results(const wifi_scan_ap_t *aps, size_t count);

static bool link_up(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

/* RSSI активной точки; 0 — связи нет. */
int ha_wifi_rssi(int *rssi)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    if (rssi != NULL) {
        *rssi = ap.rssi;
    }
    return 1;
}

static void schedule_reconnect(void)
{
    if (s_reconnect_pending || s_queue == NULL) {
        return;
    }
    s_reconnect_pending = true;
    const wifi_req_t request = {.kind = WIFI_REQ_RECONNECT};
    if (xQueueSend(s_queue, &request, 0) != pdTRUE) {
        s_reconnect_pending = false;
    }
}

/* --- Known APs ----------------------------------------------------------- */

static bool ssid_equals(const char *a, const char *b)
{
    return strncmp(a, b, WIFI_SSID_MAX) == 0;
}

static bool known_add_raw(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    const char *pass = password != NULL ? password : "";
    for (size_t i = 0; i < s_known_count; ++i) {
        if (ssid_equals(s_known[i].ssid, ssid)) {
            if (strncmp(s_known[i].password, pass, WIFI_PASSWORD_MAX) == 0) {
                return false;
            }
            strlcpy(s_known[i].password, pass, sizeof(s_known[i].password));
            return true;
        }
    }
    if (s_known_count >= WIFI_KNOWN_MAX) {
        ESP_LOGW(TAG, "known list full, \"%s\" not saved", ssid);
        return false;
    }
    strlcpy(s_known[s_known_count].ssid, ssid, sizeof(s_known[0].ssid));
    strlcpy(s_known[s_known_count].password, pass, sizeof(s_known[0].password));
    s_known_count++;
    return true;
}

static void known_add(const char *ssid, const char *password)
{
    (void)known_add_raw(ssid, password);
}

/* Domain owns persistence; the radio only keeps the RAM list. */
int ha_wifi_known_add(const char *ssid, const char *password)
{
    return known_add_raw(ssid, password) ? 0 : -1;
}

int ha_wifi_known_count(void)
{
    return (int)s_known_count;
}

int ha_wifi_known_get(int index, char *ssid, size_t len, char *password, size_t password_len)
{
    if (index < 0 || (size_t)index >= s_known_count || ssid == NULL) {
        return 0;
    }
    strlcpy(ssid, s_known[index].ssid, len);
    if (password != NULL) {
        strlcpy(password, s_known[index].password, password_len);
    }
    return 1;
}

/* --- Radio --------------------------------------------------------------- */

static bool scan_now(wifi_scan_ap_t *aps, size_t *count)
{
    wifi_scan_config_t config = {0};
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 120;
    config.scan_time.active.max = 300;
    const esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK) {
        found = 0;
    }
    wifi_ap_record_t records[WIFI_SCAN_MAX] = {0};
    uint16_t shown = (found > WIFI_SCAN_MAX) ? WIFI_SCAN_MAX : found;
    if (shown > 0 && esp_wifi_scan_get_ap_records(&shown, records) != ESP_OK) {
        return false;
    }

    for (uint16_t i = 0; i < shown; ++i) {
        strlcpy(aps[i].ssid, (const char *)records[i].ssid, sizeof(aps[i].ssid));
        aps[i].rssi = records[i].rssi;
        memcpy(aps[i].bssid, records[i].bssid, sizeof(aps[i].bssid));
        aps[i].channel = records[i].primary;
        ESP_LOGI(TAG, "scan: \"%s\" %d dBm ch=%u auth=%d", aps[i].ssid, (int)aps[i].rssi,
                 (unsigned)aps[i].channel, (int)records[i].authmode);
    }
    *count = shown;
    return true;
}

/* Strongest known AP in range. */
static bool select_known(const wifi_scan_ap_t *aps, size_t count, size_t *out_index,
                         int8_t *out_rssi, size_t *out_ap)
{
    bool found = false;
    int8_t best_rssi = 0;
    size_t best_index = 0;
    size_t best_ap = 0;
    for (size_t i = 0; i < count; ++i) {
        for (size_t k = 0; k < s_known_count; ++k) {
            if (!ssid_equals(aps[i].ssid, s_known[k].ssid)) {
                continue;
            }
            if (!found || aps[i].rssi > best_rssi) {
                found = true;
                best_rssi = aps[i].rssi;
                best_index = k;
                best_ap = i;
            }
            break;
        }
    }
    if (!found) {
        return false;
    }
    *out_index = best_index;
    *out_rssi = best_rssi;
    *out_ap = best_ap;
    return true;
}

/* bssid/channel pin the connect to the exact AP just seen in the scan. */
static void do_connect_ap(const char *ssid, const char *password, const wifi_scan_ap_t *ap)
{
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    /* Connect by SSID; prefer the scanned BSSID/channel when available. */
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (ap != NULL && ap->channel != 0) {
        config.sta.bssid_set = true;
        memcpy(config.sta.bssid, ap->bssid, sizeof(config.sta.bssid));
        config.sta.channel = ap->channel;
        config.sta.scan_method = WIFI_FAST_SCAN;
    } else {
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    }

    /* WPA2 minimum; PMF capable (not required); SAE H2E for WPA3. */
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    s_connected = false;
    s_last_connect_at = xTaskGetTickCount();
    /* Clear a half-association, then let the driver leave the old state before
     * issuing a new connect (an immediate connect returns ESP_ERR_WIFI_STATE). */
    if (esp_wifi_disconnect() == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
        ESP_LOGW(TAG, "set_config failed for \"%s\"", ssid);
        schedule_reconnect();
        return;
    }
    if (ap != NULL) {
        ESP_LOGI(TAG, "connecting to \"%s\" bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u", ssid,
                 ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4],
                 ap->bssid[5], (unsigned)ap->channel);
    } else {
        ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s; retry", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            schedule_reconnect(); /* no DISCONNECTED event will follow */
        }
    }
}

static void do_connect(const char *ssid, const char *password)
{
    do_connect_ap(ssid, password, NULL);
}

/* Rescan and reconnect to the strongest known AP. */
static void autoconnect(void)
{
    if (link_up()) {
        return;
    }
    if (s_known_count == 0) {
        ESP_LOGW(TAG, "no known networks; nothing to connect to");
        return;
    }

    wifi_scan_ap_t aps[WIFI_SCAN_MAX] = {0};
    size_t count = 0;
    if (!scan_now(aps, &count)) {
        schedule_reconnect();
        return;
    }
    store_scan_results(aps, count);

    size_t index = 0;
    size_t ap_index = 0;
    int8_t rssi = 0;
    if (!select_known(aps, count, &index, &rssi, &ap_index)) {
        ESP_LOGI(TAG, "no known network in range; will rescan");
        schedule_reconnect();
        return;
    }
    ESP_LOGI(TAG, "autoconnect to \"%s\" (%d dBm)", s_known[index].ssid, (int)rssi);
    do_connect_ap(s_known[index].ssid, s_known[index].password, &aps[ap_index]);
}

static void do_reconnect(void)
{
    const uint32_t shift = s_fail_count > RECONNECT_MAX_SHIFT ? RECONNECT_MAX_SHIFT : s_fail_count;
    if (s_fail_count < UINT32_MAX) {
        s_fail_count++;
    }
    const uint32_t delay_ms = 1000u << shift;
    ESP_LOGI(TAG, "reconnect in %u ms (attempt %u)", (unsigned)delay_ms, (unsigned)s_fail_count);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    s_reconnect_pending = false;
    if (link_up()) {
        return;
    }
    autoconnect();
}

/* DISCONNECTED can be lost (stuck stack, lost hosted RPC), so check the AP every
 * 5 s: two misses in a row mean the link is gone. While connect/DHCP is in
 * progress (15 s after the request) the watchdog stays out. */
static void wifi_watchdog(void)
{
    static uint8_t misses;

    if (s_known_count == 0) {
        return;
    }
    if (s_connected) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            misses = 0;
            return;
        }
        if (++misses < 2) {
            return;
        }
        misses = 0;
        ESP_LOGW(TAG, "watchdog: AP info lost; reconnect");
        s_connected = false;
        schedule_reconnect();
        return;
    }
    misses = 0;
    if (s_reconnect_pending) {
        return;
    }
    if ((xTaskGetTickCount() - s_last_connect_at) < pdMS_TO_TICKS(WIFI_RECONNECT_GRACE_MS)) {
        return;
    }
    ESP_LOGW(TAG, "watchdog: not connected; reconnect");
    schedule_reconnect();
}

/* A bare reason number does not diagnose anything. */
static const char *reason_name(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE (AP did not answer auth)";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "ASSOC_EXPIRE (AP did not answer assoc)";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "ASSOC_TOOMANY (AP client limit)";
    case WIFI_REASON_ASSOC_LEAVE:
        return "ASSOC_LEAVE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT (wrong key/password)";
    case WIFI_REASON_STA_LEAVING:
        return "STA_LEAVING";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL (AP rejected auth)";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:
        return "?";
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected: reason=%d (%s) rssi=%d",
                 event ? (int)event->reason : -1,
                 event ? reason_name((int)event->reason) : "?", event ? (int)event->rssi : 0);
        s_connected = false;
        schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_reconnect_pending = false;
        s_fail_count = 0; /* link is up: reset the backoff */
    }
}

/* Cache the latest scan (autoconnect and manual share it for the UI). */
static void store_scan_results(const wifi_scan_ap_t *aps, size_t count)
{
    if (s_scan_mutex != NULL) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    s_scan_count = count;
    if (count > 0 && aps != NULL) {
        memcpy(s_scan_results, aps, count * sizeof(aps[0]));
    }
    if (s_scan_mutex != NULL) {
        xSemaphoreGive(s_scan_mutex);
    }
}

static void scan_and_store(void)
{
    /* A scan in the middle of auth/assoc returns no APs; idle the radio first. */
    if (!link_up()) {
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    wifi_scan_ap_t aps[WIFI_SCAN_MAX] = {0};
    size_t count = 0;
    const bool ok = scan_now(aps, &count);
    if (ok) {
        store_scan_results(aps, count);
    }
    /* Keep the last scan when a scan fails. */
    if (s_scan_mutex != NULL) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    s_scan_ready = true;
    const size_t visible = s_scan_count;
    if (s_scan_mutex != NULL) {
        xSemaphoreGive(s_scan_mutex);
    }
    ESP_LOGI(TAG, "scan done: %u networks (ok=%d)", (unsigned)visible, (int)ok);
}

static void wifi_task(void *arg)
{
    (void)arg;
    autoconnect();
    for (;;) {
        wifi_req_t request;
        if (xQueueReceive(s_queue, &request, pdMS_TO_TICKS(WIFI_WATCHDOG_MS)) == pdTRUE) {
            switch (request.kind) {
            case WIFI_REQ_SCAN:
                scan_and_store();
                break;
            case WIFI_REQ_CONNECT:
                s_reconnect_pending = false;
                known_add(request.ssid, request.password);
                do_connect(request.ssid, request.password);
                break;
            default:
                if (s_connected) {
                    s_reconnect_pending = false; /* stale request: link is back */
                } else {
                    do_reconnect();
                }
                break;
            }
        } else {
            wifi_watchdog();
        }
    }
}

static esp_err_t wifi_init_stack(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    (void)esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    (void)esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);

    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK || (err = esp_wifi_start()) != ESP_OK) {
        return err;
    }
    /* Power save drops auth frames even with a strong signal. */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_wifi_set_country_code("RU", true);
    const int8_t max_tx_power = WIFI_MAX_TX_POWER;
    const esp_err_t tx_err = esp_wifi_set_max_tx_power(max_tx_power);
    ESP_LOGI(TAG, "max tx power=%d (%.1f dBm): %s", max_tx_power,
             (double)max_tx_power * 0.25, esp_err_to_name(tx_err));
    return ESP_OK;
}

esp_err_t ha_wifi_start(void)
{
    if (s_task != NULL) {
        return ESP_OK; /* already started */
    }
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK && nvs_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "nvs init failed: %s; known APs will not persist",
                 esp_err_to_name(nvs_err));
    }
    const esp_err_t err = wifi_init_stack();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t sta_mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, sta_mac) == ESP_OK) {
        ESP_LOGI(TAG, "sta mac=%02x:%02x:%02x:%02x:%02x:%02x", sta_mac[0], sta_mac[1],
                 sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    }

    s_queue = xQueueCreate(4, sizeof(wifi_req_t));
    s_scan_mutex = xSemaphoreCreateMutex();
    if (s_queue == NULL || s_scan_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(wifi_task, "ha_wifi", WIFI_TASK_STACK, NULL, WIFI_TASK_PRIORITY, &s_task) !=
        pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --- UI provisioning (scan / connect / status) ------------------------- */

/* Non-blocking: the Wi-Fi task performs the scan; the caller polls ready. */
int ha_wifi_scan_start(void)
{
    if (s_queue == NULL) {
        return -1;
    }
    if (s_scan_mutex != NULL) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    s_scan_ready = false;
    if (s_scan_mutex != NULL) {
        xSemaphoreGive(s_scan_mutex);
    }
    const wifi_req_t request = {.kind = WIFI_REQ_SCAN};
    return xQueueSend(s_queue, &request, 0) == pdTRUE ? 0 : -1;
}

int ha_wifi_scan_ready(void)
{
    int ready = 0;
    if (s_scan_mutex != NULL) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    ready = s_scan_ready ? 1 : 0;
    if (s_scan_mutex != NULL) {
        xSemaphoreGive(s_scan_mutex);
    }
    return ready;
}

void ha_wifi_scan_clear(void)
{
    if (s_scan_mutex != NULL) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    s_scan_ready = false;
    if (s_scan_mutex != NULL) {
        xSemaphoreGive(s_scan_mutex);
    }
}

int ha_wifi_scan_count(void)
{
    int count = 0;
    if (s_scan_mutex != NULL) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    count = (int)s_scan_count;
    if (s_scan_mutex != NULL) {
        xSemaphoreGive(s_scan_mutex);
    }
    return count;
}

int ha_wifi_scan_get(int index, char *ssid, size_t len, int *rssi)
{
    int ok = 0;
    if (index < 0 || ssid == NULL) {
        return 0;
    }
    if (s_scan_mutex != NULL) {
        xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    }
    if ((size_t)index < s_scan_count) {
        strlcpy(ssid, s_scan_results[index].ssid, len);
        if (rssi != NULL) {
            *rssi = s_scan_results[index].rssi;
        }
        ok = 1;
    }
    if (s_scan_mutex != NULL) {
        xSemaphoreGive(s_scan_mutex);
    }
    return ok;
}

int ha_wifi_connect_ssid(const char *ssid, const char *password)
{
    if (s_queue == NULL || ssid == NULL || ssid[0] == '\0') {
        return -1;
    }
    wifi_req_t request = {.kind = WIFI_REQ_CONNECT};
    strlcpy(request.ssid, ssid, sizeof(request.ssid));
    strlcpy(request.password, password != NULL ? password : "", sizeof(request.password));
    return xQueueSend(s_queue, &request, 0) == pdTRUE ? 0 : -1;
}

/* 1 — associated; ssid/rssi filled. */
int ha_wifi_status(char *ssid, size_t len, int *rssi)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    if (ssid != NULL) {
        strlcpy(ssid, (const char *)ap.ssid, len);
    }
    if (rssi != NULL) {
        *rssi = ap.rssi;
    }
    return 1;
}

/* Start SNTP once the STA has an address. V3 is local-first; UTC is the base
 * until a location/timezone service exists. */
void ha_time_start(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

int ha_wifi_ip(char *out, size_t len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return 0;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return 0;
    }
    snprintf(out, len, IPSTR, IP2STR(&info.ip));
    return 1;
}
