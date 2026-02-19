#include "gw_wifi_sta.h"

#include <string.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw_core/net_time.h"
#include "gw_core/weather.h"

static const char *TAG = "gw_wifi_sta";

static bool s_started = false;
static bool s_connected = false;
static esp_netif_t *s_sta_netif = NULL;
static char s_last_ssid[33];
static char s_last_password[65];
static TaskHandle_t s_connect_task = NULL;

static bool find_best_ap_by_ssid(const char *ssid, wifi_ap_record_t *out_ap, uint16_t *out_visible)
{
    if (out_visible) {
        *out_visible = 0;
    }
    if (!ssid || !ssid[0] || !out_ap) {
        return false;
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan get_ap_num failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t fetch_num = (ap_num > 64) ? 64 : ap_num;
    if (out_visible) {
        *out_visible = fetch_num;
    }
    if (fetch_num == 0) {
        return false;
    }

    wifi_ap_record_t *records = (wifi_ap_record_t *)calloc(fetch_num, sizeof(wifi_ap_record_t));
    if (!records) {
        ESP_LOGW(TAG, "scan no mem");
        return false;
    }
    err = esp_wifi_scan_get_ap_records(&fetch_num, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan get_ap_records failed: %s", esp_err_to_name(err));
        free(records);
        return false;
    }

    bool found = false;
    int best_rssi = -127;
    wifi_ap_record_t best = {0};
    for (uint16_t i = 0; i < fetch_num; i++) {
        if (strncmp((const char *)records[i].ssid, ssid, 32) == 0) {
            if (!found || records[i].rssi > best_rssi) {
                found = true;
                best_rssi = records[i].rssi;
                best = records[i];
            }
        }
    }
    free(records);
    if (!found) {
        return false;
    }
    *out_ap = best;
    return true;
}

static void wifi_connect_pipeline_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (s_started && !s_connected) {
            if (s_last_ssid[0] == '\0') {
                break;
            }

            wifi_ap_record_t ap = {0};
            uint16_t visible_ap = 0;
            bool found = find_best_ap_by_ssid(s_last_ssid, &ap, &visible_ap);
            if (!found) {
                ESP_LOGW(TAG, "pipeline: ssid=\"%s\" not found (visible_ap=%u), retry in 3s", s_last_ssid, (unsigned)visible_ap);
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }

            ESP_LOGI(TAG,
                     "pipeline: ssid=\"%s\" found ch=%u rssi=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                     s_last_ssid, (unsigned)ap.primary, ap.rssi,
                     ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);

            wifi_config_t wifi_cfg = {0};
            strlcpy((char *)wifi_cfg.sta.ssid, s_last_ssid, sizeof(wifi_cfg.sta.ssid));
            strlcpy((char *)wifi_cfg.sta.password, s_last_password, sizeof(wifi_cfg.sta.password));
            wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
            wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
            wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
            wifi_cfg.sta.pmf_cfg.capable = true;
            wifi_cfg.sta.pmf_cfg.required = false;
            wifi_cfg.sta.channel = ap.primary;
            wifi_cfg.sta.bssid_set = 1;
            memcpy(wifi_cfg.sta.bssid, ap.bssid, sizeof(wifi_cfg.sta.bssid));

            esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "pipeline: set_config failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            (void)esp_wifi_disconnect();
            err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "pipeline: connect failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            for (int i = 0; i < 40; i++) { // ~8s
                if (s_connected) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (!s_connected) {
                ESP_LOGW(TAG, "pipeline: connect timeout for ssid=\"%s\", retrying", s_last_ssid);
            }
        }
    }
}

static void on_wifi_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA start event");
        if (s_connect_task) {
            (void)xTaskNotifyGive(s_connect_task);
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        ESP_LOGW(TAG, "STA disconnected, reason=%u", disc ? (unsigned)disc->reason : 0U);
        if (disc && disc->reason == WIFI_REASON_NO_AP_FOUND) {
            ESP_LOGW(TAG, "No AP found for ssid=\"%s\"", s_last_ssid);
        }
        if (s_connect_task) {
            (void)xTaskNotifyGive(s_connect_task);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_id != IP_EVENT_STA_GOT_IP || !event_data) {
        return;
    }
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)event_data;
    s_connected = true;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));

    esp_err_t t_err = gw_net_time_request_sync();
    if (t_err != ESP_OK && t_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Immediate time sync request failed: %s", esp_err_to_name(t_err));
    }
    esp_err_t w_err = gw_weather_request_refresh();
    if (w_err != ESP_OK && w_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Immediate weather refresh request failed: %s", esp_err_to_name(w_err));
    }

    // If DHCP did not provide DNS, set pragmatic fallbacks.
    if (s_sta_netif) {
        esp_netif_dns_info_t dns = {0};
        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            if (dns.ip.u_addr.ip4.addr == 0) {
                esp_netif_dns_info_t d1 = {0};
                esp_netif_dns_info_t d2 = {0};
                d1.ip.type = ESP_IPADDR_TYPE_V4;
                d2.ip.type = ESP_IPADDR_TYPE_V4;
                d1.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
                d2.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
                (void)esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &d1);
                (void)esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &d2);
                ESP_LOGW(TAG, "DNS fallback applied: 8.8.8.8 / 1.1.1.1");
            }
        }
    }
}

esp_err_t gw_wifi_sta_start(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || !password) {
        ESP_LOGW(TAG, "Wi-Fi credentials are empty");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_last_ssid, ssid, sizeof(s_last_ssid));
    strlcpy(s_last_password, password, sizeof(s_last_password));

    if (s_started) {
        s_connected = false;
        if (s_connect_task) {
            (void)xTaskNotifyGive(s_connect_task);
        }
        return ESP_OK;
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    // Enable world roaming domain (1..13) + 802.11d hints.
    // This helps when AP is on channels 12/13 (common outside US defaults).
    err = esp_wifi_set_country_code("01", true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_country_code failed: %s", esp_err_to_name(err));
    } else {
        wifi_country_t country = {0};
        if (esp_wifi_get_country(&country) == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi country=%c%c schan=%u nchan=%u policy=%u",
                     country.cc[0], country.cc[1],
                     (unsigned)country.schan, (unsigned)country.nchan, (unsigned)country.policy);
        }
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_connect_task) {
        if (xTaskCreate(wifi_connect_pipeline_task, "wifi_conn_pipe", 4608, NULL, 3, &s_connect_task) != pdPASS) {
            s_connect_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    s_started = true;
    ESP_LOGI(TAG, "Wi-Fi STA started");
    (void)xTaskNotifyGive(s_connect_task);
    return ESP_OK;
}

bool gw_wifi_sta_is_connected(void)
{
    return s_connected;
}
