#include "gw_wifi_sta.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_ip_addr.h"

static const char *TAG = "gw_wifi_sta";

static bool s_started = false;
static bool s_connected = false;
static esp_netif_t *s_sta_netif = NULL;

static void on_wifi_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        (void)esp_wifi_connect();
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
    if (s_started) {
        return ESP_OK;
    }
    if (!ssid || ssid[0] == '\0' || !password) {
        ESP_LOGW(TAG, "Wi-Fi credentials are empty");
        return ESP_ERR_INVALID_ARG;
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

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "Wi-Fi STA started");
    return ESP_OK;
}

bool gw_wifi_sta_is_connected(void)
{
    return s_connected;
}
