#include "wifi/wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_wifi.h"
#include "wifi/wifi_select.h"

/*
 * Wi-Fi service (docs/services/WIFI.md). Один владелец радио (C3 через ESP-Hosted):
 * подъём стека, скан, подключение, автоподключение по известным. Заполняет Domain
 * (wifi_scan/known/status) и исполняет команды HA_CMD_WIFI_SCAN/CONNECT.
 */

#define TAG "wifi"
/* Скан/автоподключение держат на стеке крупные массивы AP/known — 4 КБ мало. */
#define WIFI_TASK_STACK 8192
#define WIFI_TASK_PRIORITY 4
#define WIFI_QUEUE_LEN 4

typedef enum {
    WIFI_REQ_SCAN = 0,
    WIFI_REQ_CONNECT = 1,
    WIFI_REQ_RECONNECT = 2,
} wifi_req_kind_t;

typedef struct {
    uint8_t kind;
    ha_wifi_connect_args_t args;
} wifi_request_t;

typedef struct {
    ha_wifi_known_key_t key;
    ha_wifi_known_record_t record;
} known_entry_t;

static domain_t *s_domain;
static QueueHandle_t s_queue;
/* Глушит автопереподключение на время скана: STA должен быть свободен. */
static volatile bool s_reconnect_enabled = true;
/* Счётчик подряд неудачных переподключений — задаёт backoff (AP отбивает частые ретраи). */
static uint32_t s_fail_count = 0;

static sys_error_t wifi_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_WIFI, code);
}

/* --- Domain: статус / скан / известные ---------------------------------- */

static void put_status(uint8_t state, uint8_t connected, int8_t rssi, const char *ssid)
{
    ha_wifi_status_record_t record = {0};
    record.state = state;
    record.connected = connected;
    record.rssi = rssi;
    if (ssid != NULL) {
        strlcpy(record.ssid, ssid, sizeof(record.ssid));
    }
    const ha_device_id_t uid = HA_WIFI_DEVICE_ID;
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_WIFI;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_WIFI_STATUS, &uid, &record, &meta,
                            &changed);
}

static void put_scan(const ha_wifi_scan_record_t *scan)
{
    const ha_device_id_t uid = HA_WIFI_DEVICE_ID;
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_WIFI;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_WIFI_SCAN, &uid, scan, &meta,
                            &changed);
}

static void clear_scan(void)
{
    const ha_device_id_t uid = HA_WIFI_DEVICE_ID;
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_WIFI;
    (void)domain_entity_remove(s_domain, (domain_entity_t)HA_ENTITY_WIFI_SCAN, &uid, &meta);
}

typedef struct {
    known_entry_t *cursor;
    known_entry_t *end;
} known_collect_t;

static bool known_cb(const void *key, const void *record, void *ctx)
{
    known_collect_t *collect = (known_collect_t *)ctx;
    if (collect->cursor >= collect->end) {
        return false; /* буфер полон — дальше не собираем */
    }
    collect->cursor->key = *(const ha_wifi_known_key_t *)key;
    collect->cursor->record = *(const ha_wifi_known_record_t *)record;
    collect->cursor++;
    return true;
}

/* Собирает известные точки в буфер; возвращает число записей (не больше capacity). */
static size_t collect_known(known_entry_t *out, size_t capacity)
{
    if (capacity == 0) {
        return 0;
    }
    known_collect_t collect = {.cursor = out, .end = out + capacity};
    (void)domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_WIFI_KNOWN, known_cb, &collect);
    return (size_t)(collect.cursor - out);
}

static bool ssid_equals(const char *a, const char *b)
{
    return strncmp(a, b, HA_WIFI_SSID_MAX) == 0;
}

/* Сохраняет ssid+password: обновляет запись, если ssid уже известен, иначе — новый id. */
static void ensure_known(const char *ssid, const char *password)
{
    known_entry_t entries[HA_WIFI_KNOWN_MAX] = {0};
    const size_t count = collect_known(entries, HA_WIFI_KNOWN_MAX);

    uint32_t max_id = 0;
    bool has_any = false;
    for (size_t i = 0; i < count; ++i) {
        if (ssid_equals(entries[i].record.ssid, ssid)) {
            ha_wifi_known_record_t record = entries[i].record;
            strlcpy(record.password, password, sizeof(record.password));
            domain_fact_meta_t meta = {0};
            meta.source = (uint8_t)DOMAIN_SOURCE_WIFI;
            bool changed = false;
            (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_WIFI_KNOWN,
                                    &entries[i].key, &record, &meta, &changed);
            return;
        }
        if (!has_any || entries[i].key.id >= max_id) {
            max_id = entries[i].key.id;
            has_any = true;
        }
    }

    if (count >= HA_WIFI_KNOWN_MAX) {
        ESP_LOGW(TAG, "known list full, \"%s\" not saved", ssid);
        return;
    }
    const ha_wifi_known_key_t key = {.id = has_any ? (max_id + 1u) : 1u};
    ha_wifi_known_record_t record = {0};
    strlcpy(record.ssid, ssid, sizeof(record.ssid));
    strlcpy(record.password, password, sizeof(record.password));
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_WIFI;
    bool changed = false;
    (void)domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_WIFI_KNOWN, &key, &record, &meta,
                            &changed);
}

/* --- Радио -------------------------------------------------------------- */

static uint8_t map_auth(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return HA_WIFI_AUTH_OPEN;
    case WIFI_AUTH_WPA_PSK:
        return HA_WIFI_AUTH_WPA;
    case WIFI_AUTH_WPA2_PSK:
        return HA_WIFI_AUTH_WPA2;
    case WIFI_AUTH_WPA3_PSK:
        return HA_WIFI_AUTH_WPA3;
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return HA_WIFI_AUTH_WPA2_ENTERPRISE;
    default:
        return HA_WIFI_AUTH_WPA2;
    }
}

/* Скан в запись; true — получилось (даже если сетей нет). */
static bool scan_now(ha_wifi_scan_record_t *out)
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
    wifi_ap_record_t records[HA_WIFI_SCAN_MAX] = {0};
    uint16_t shown = (found > HA_WIFI_SCAN_MAX) ? HA_WIFI_SCAN_MAX : found;
    if (shown > 0 && esp_wifi_scan_get_ap_records(&shown, records) != ESP_OK) {
        return false;
    }

    ha_wifi_scan_record_t scan = {0};
    scan.count = (uint8_t)shown;
    for (uint16_t i = 0; i < shown; ++i) {
        strlcpy(scan.aps[i].ssid, (const char *)records[i].ssid, sizeof(scan.aps[i].ssid));
        scan.aps[i].rssi = records[i].rssi;
        scan.aps[i].auth = map_auth(records[i].authmode);
    }
    *out = scan;
    return true;
}

static void do_scan(void)
{
    put_status(HA_WIFI_STATE_SCANNING, 0, 0, NULL);

    /* Скан требует свободного STA: снимаем текущее подключение и глушим
     * автопереподключение, иначе esp_wifi_scan_start вернёт ESP_ERR_WIFI_STATE. */
    s_reconnect_enabled = false;
    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));

    ha_wifi_scan_record_t scan = {0};
    const bool ok = scan_now(&scan);
    s_reconnect_enabled = true;
    if (!ok) {
        put_status(HA_WIFI_STATE_ERROR, 0, 0, NULL);
        return;
    }
    put_scan(&scan);
    put_status(HA_WIFI_STATE_IDLE, 0, 0, NULL);
}

static void do_connect(const char *ssid, const char *password)
{
    ensure_known(ssid, password);

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    /* Как в старом рабочем web-сервисе: порог WPA2. */
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* PMF не обязателен, но заявляем поддержку: часть AP иначе рвёт ассоциацию. */
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    put_status(HA_WIFI_STATE_CONNECTING, 0, 0, ssid);
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
        put_status(HA_WIFI_STATE_ERROR, 0, 0, ssid);
        return;
    }
    esp_wifi_connect();
}

/* Автоподключение при старте: известная точка с самым сильным сигналом. */
static void autoconnect(void)
{
    known_entry_t entries[HA_WIFI_KNOWN_MAX] = {0};
    const size_t known_count = collect_known(entries, HA_WIFI_KNOWN_MAX);
    if (known_count == 0) {
        put_status(HA_WIFI_STATE_IDLE, 0, 0, NULL);
        return;
    }

    ha_wifi_scan_record_t scan = {0};
    if (!scan_now(&scan)) {
        put_status(HA_WIFI_STATE_ERROR, 0, 0, NULL);
        return;
    }

    ha_wifi_known_record_t known[HA_WIFI_KNOWN_MAX] = {0};
    for (size_t i = 0; i < known_count; ++i) {
        known[i] = entries[i].record;
    }

    size_t index = 0;
    int8_t rssi = 0;
    if (wifi_select_known(&scan, known, known_count, &index, &rssi)) {
        ESP_LOGI(TAG, "autoconnect to \"%s\" (%d dBm)", known[index].ssid, (int)rssi);
        do_connect(known[index].ssid, known[index].password);
    } else {
        ESP_LOGI(TAG, "no known network in range");
        put_status(HA_WIFI_STATE_IDLE, 0, 0, NULL);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected: reason=%d rssi=%d", event ? (int)event->reason : -1,
                 event ? (int)event->rssi : 0);
        put_status(HA_WIFI_STATE_ERROR, 0, 0, NULL);
        if (s_reconnect_enabled && s_queue != NULL) {
            wifi_request_t request = {.kind = WIFI_REQ_RECONNECT};
            (void)xQueueSend(s_queue, &request, 0); /* backoff — в задаче, чтобы не долбить AP */
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_ap_record_t ap = {0};
        const char *ssid = "";
        int8_t rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ssid = (const char *)ap.ssid;
            rssi = ap.rssi;
        }
        put_status(HA_WIFI_STATE_CONNECTED, 1, rssi, ssid);
        s_fail_count = 0; /* связь есть — сбрасываем backoff */
        clear_scan(); /* скан больше не нужен — подключение состоялось */
    }
}

static sys_error_t wifi_init_stack(void)
{
    (void)esp_netif_init();
    (void)esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) {
        return wifi_fail(SYS_CODE_IO);
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        return wifi_fail(SYS_CODE_IO);
    }

    /* Без этого STA с дефолтным power-save пропускает auth-кадры и получает
     * reason=2 (AUTH_EXPIRE) при хорошем RSSI (прецедент — старый web-сервис). */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    (void)esp_wifi_set_country_code("RU", true);
    return SYS_OK;
}

/* --- Исполнители команд ------------------------------------------------- */

static sys_error_t cmd_scan(domain_command_t type, const void *args, size_t args_size, void *ctx)
{
    (void)type;
    (void)args;
    (void)args_size;
    (void)ctx;
    if (s_queue == NULL) {
        return wifi_fail(SYS_CODE_INVALID_STATE);
    }
    const wifi_request_t request = {.kind = WIFI_REQ_SCAN};
    return (xQueueSend(s_queue, &request, 0) == pdTRUE) ? SYS_OK : wifi_fail(SYS_CODE_BUSY);
}

static sys_error_t cmd_connect(domain_command_t type, const void *args, size_t args_size, void *ctx)
{
    (void)type;
    (void)ctx;
    if (args == NULL || args_size != sizeof(ha_wifi_connect_args_t)) {
        return wifi_fail(SYS_CODE_INVALID_SIZE);
    }
    if (s_queue == NULL) {
        return wifi_fail(SYS_CODE_INVALID_STATE);
    }
    wifi_request_t request = {.kind = WIFI_REQ_CONNECT};
    request.args = *(const ha_wifi_connect_args_t *)args;
    return (xQueueSend(s_queue, &request, 0) == pdTRUE) ? SYS_OK : wifi_fail(SYS_CODE_BUSY);
}

/* --- Задача ------------------------------------------------------------- */

/* Переподключение с экспоненциальным backoff (1,2,4,8,16 c): мгновенные ретраи AP
 * воспринимает как флуд и отвечает reason=2 (AUTH_EXPIRE) даже при сильном сигнале. */
static void do_reconnect(void)
{
    uint32_t shift = s_fail_count;
    if (shift > 4) {
        shift = 4;
    }
    const uint32_t delay_ms = 1000u << shift;
    if (s_fail_count < UINT32_MAX) {
        s_fail_count++;
    }
    ESP_LOGI(TAG, "reconnect in %u ms (attempt %u)", (unsigned)delay_ms, (unsigned)s_fail_count);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    if (s_reconnect_enabled) {
        esp_wifi_connect();
    }
}

static void wifi_task(void *arg)
{
    s_domain = (domain_t *)arg;
    if (sys_failed(wifi_init_stack())) {
        ESP_LOGE(TAG, "Wi-Fi stack init failed");
        vTaskDelete(NULL);
        return;
    }
    autoconnect();

    for (;;) {
        wifi_request_t request = {0};
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (request.kind == WIFI_REQ_SCAN) {
            do_scan();
        } else if (request.kind == WIFI_REQ_RECONNECT) {
            do_reconnect();
        } else {
            do_connect(request.args.ssid, request.args.password);
        }
    }
}

sys_error_t wifi_start(domain_t *domain)
{
    if (domain == NULL) {
        return wifi_fail(SYS_CODE_INVALID_ARG);
    }
    s_domain = domain;
    s_queue = xQueueCreate(WIFI_QUEUE_LEN, sizeof(wifi_request_t));
    if (s_queue == NULL) {
        return wifi_fail(SYS_CODE_NO_MEM);
    }

    const sys_error_t scan_err = domain_register_command(domain, HA_CMD_WIFI_SCAN, cmd_scan, NULL);
    if (sys_failed(scan_err)) {
        return scan_err;
    }
    const sys_error_t connect_err =
        domain_register_command(domain, HA_CMD_WIFI_CONNECT, cmd_connect, NULL);
    if (sys_failed(connect_err)) {
        return connect_err;
    }

    if (xTaskCreate(wifi_task, "wifi", WIFI_TASK_STACK, domain, WIFI_TASK_PRIORITY, NULL) != pdPASS) {
        return wifi_fail(SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
