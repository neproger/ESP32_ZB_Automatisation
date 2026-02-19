#include "gw_core/net_time.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static const char *TAG = "gw_net_time";

static const uint32_t kDefaultSyncIntervalMs = 6u * 60u * 60u * 1000u;
static const uint32_t kDefaultSyncTimeoutMs = 8000u;
static const char *kDefaultNtpServer = "pool.ntp.org";
static const uint32_t kTimeTaskStackWords = 3072;

static TaskHandle_t s_task = NULL;
static bool s_initialized = false;
static bool s_synced = false;
static bool s_sntp_inited = false;
static uint64_t s_epoch_ref_ms = 0;
static uint64_t s_mono_ref_ms = 0;
static uint64_t s_last_sync_epoch_ms = 0;
static gw_net_time_cfg_t s_cfg = {0};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint64_t mono_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static void update_ref_from_system_time(void)
{
    struct timeval tv = {0};
    if (gettimeofday(&tv, NULL) != 0) {
        return;
    }
    if (tv.tv_sec <= 0) {
        return;
    }

    const uint64_t epoch_ms = ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
    const uint64_t mono_ms = mono_now_ms();

    portENTER_CRITICAL(&s_lock);
    s_epoch_ref_ms = epoch_ms;
    s_mono_ref_ms = mono_ms;
    s_last_sync_epoch_ms = epoch_ms;
    s_synced = true;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t perform_sync_once(void)
{
    if (!s_sntp_inited) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_cfg.ntp_server ? s_cfg.ntp_server : kDefaultNtpServer);
        cfg.start = true;
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_sntp_inited = true;
    } else {
        esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_netif_sntp_start failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    const TickType_t wait_ticks = pdMS_TO_TICKS((s_cfg.sync_timeout_ms > 0) ? s_cfg.sync_timeout_ms : kDefaultSyncTimeoutMs);
    esp_err_t err = esp_netif_sntp_sync_wait(wait_ticks);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync timeout/fail: %s", esp_err_to_name(err));
        return err;
    }

    update_ref_from_system_time();
    ESP_LOGI(TAG, "time synced, epoch_ms=%llu", (unsigned long long)gw_net_time_last_sync_ms());
    return ESP_OK;
}

static void time_task(void *arg)
{
    (void)arg;

    if (s_cfg.sync_on_init) {
        (void)perform_sync_once();
    }

    const TickType_t interval_ticks = pdMS_TO_TICKS((s_cfg.sync_interval_ms > 0) ? s_cfg.sync_interval_ms : kDefaultSyncIntervalMs);
    for (;;) {
        const uint32_t sig = ulTaskNotifyTake(pdTRUE, interval_ticks);
        if (sig > 0) {
            (void)perform_sync_once();
            continue;
        }
        (void)perform_sync_once();
    }
}

esp_err_t gw_net_time_init(const gw_net_time_cfg_t *cfg)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    if (cfg) {
        s_cfg = *cfg;
    }

    if (!s_cfg.ntp_server || s_cfg.ntp_server[0] == '\0') {
        s_cfg.ntp_server = kDefaultNtpServer;
    }
    if (s_cfg.sync_interval_ms == 0) {
        s_cfg.sync_interval_ms = kDefaultSyncIntervalMs;
    }
    if (s_cfg.sync_timeout_ms == 0) {
        s_cfg.sync_timeout_ms = kDefaultSyncTimeoutMs;
    }
    if (!cfg) {
        s_cfg.sync_on_init = true;
    }

    BaseType_t ok = xTaskCreateWithCaps(
        time_task,
        "gw_time",
        kTimeTaskStackWords,
        NULL,
        2,
        &s_task,
        MALLOC_CAP_8BIT
    );
    if (ok != pdTRUE) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "initialized (server=%s interval_ms=%u)", s_cfg.ntp_server, (unsigned)s_cfg.sync_interval_ms);
    return ESP_OK;
}

esp_err_t gw_net_time_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    TaskHandle_t t = s_task;
    s_task = NULL;
    if (t) {
        vTaskDelete(t);
    }

    if (s_sntp_inited) {
        esp_netif_sntp_deinit();
        s_sntp_inited = false;
    }

    portENTER_CRITICAL(&s_lock);
    s_synced = false;
    s_epoch_ref_ms = 0;
    s_mono_ref_ms = 0;
    s_last_sync_epoch_ms = 0;
    portEXIT_CRITICAL(&s_lock);

    s_initialized = false;
    return ESP_OK;
}

bool gw_net_time_is_synced(void)
{
    bool synced = false;
    portENTER_CRITICAL(&s_lock);
    synced = s_synced;
    portEXIT_CRITICAL(&s_lock);
    return synced;
}

uint64_t gw_net_time_now_ms(void)
{
    uint64_t epoch_ref = 0;
    uint64_t mono_ref = 0;
    bool synced = false;

    portENTER_CRITICAL(&s_lock);
    epoch_ref = s_epoch_ref_ms;
    mono_ref = s_mono_ref_ms;
    synced = s_synced;
    portEXIT_CRITICAL(&s_lock);

    if (!synced) {
        return 0;
    }

    const uint64_t now_mono = mono_now_ms();
    if (now_mono < mono_ref) {
        return epoch_ref;
    }
    return epoch_ref + (now_mono - mono_ref);
}

uint64_t gw_net_time_last_sync_ms(void)
{
    uint64_t ts = 0;
    portENTER_CRITICAL(&s_lock);
    ts = s_last_sync_epoch_ms;
    portEXIT_CRITICAL(&s_lock);
    return ts;
}

esp_err_t gw_net_time_request_sync(void)
{
    if (!s_initialized || !s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ok = xTaskNotifyGive(s_task);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
