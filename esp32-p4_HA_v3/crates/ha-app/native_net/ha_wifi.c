/*
 * Платформенный адаптер Wi-Fi для ESP-Hosted: C3 остаётся slave, P4 — host.
 * WIFI_INIT_CONFIG_DEFAULT — макрос, поэтому инициализация живёт здесь, а не в FFI.
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "ha_net";

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "disconnected (reason=%d), reconnecting", event->reason);
        (void)esp_wifi_connect();
    }
}

esp_err_t ha_wifi_start(const char *ssid, const char *password)
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

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }

    esp_event_handler_instance_t instance;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL,
                                              &instance);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t config = {0};
    strncpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid) - 1);
    strncpy((char *)config.sta.password, password, sizeof(config.sta.password) - 1);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_STA, &config)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        return err;
    }
    /* V2: с дефолтным power-save STA пропускает auth-кадры -> reason=2 при хорошем RSSI. */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_wifi_set_country_code("RU", true);
    ESP_LOGI(TAG, "station started, connecting to '%s'", ssid);

    wifi_scan_config_t scan = {0};
    if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
        uint16_t count = 8;
        wifi_ap_record_t records[8];
        if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
            for (uint16_t i = 0; i < count; i++) {
                ESP_LOGI(TAG, "ap '%s' rssi=%d auth=%d", (const char *)records[i].ssid,
                         records[i].rssi, records[i].authmode);
            }
        }
    }
    return esp_wifi_connect();
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
