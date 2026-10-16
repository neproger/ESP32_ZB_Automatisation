#include "zigbee_radio_internal.h"

#include <stdbool.h>

#include "esp_log.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/zdo/zdo_nwk_mgmt.h"
#include "zigbee/zigbee_adapter_ids.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_diag.h"
#include "zigbee/zigbee_remove_mark.h"

/* BDB-сигналы стека, commissioning сети и удаление устройств (ZDO Leave). */

static const char *TAG = "zigbee.radio";

/* Запуск подпроцедуры BDB из колбэка стека. Отказ печатается, но не гасит сервис:
 * следующий сигнал или перезагрузка даст новую попытку. */
static void start_commissioning(ezb_bdb_comm_mode_mask_t mode)
{
    const ezb_err_t err = ezb_bdb_start_top_level_commissioning(mode);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "commissioning mode 0x%02x not started: code=0x%x", (unsigned)mode,
                 (unsigned)err);
    }
}

/*
 * Устройство стартовало на BDB. Factory-new (сети ещё нет) координатор её создаёт;
 * уже связанный — на старте сеть не открывает (подключение новых — только командой UI).
 */
static void commission_on_startup(void)
{
    if (ezb_bdb_is_factory_new()) {
        /* Первый запуск: сеть нужно создать, formation открывает её для steering. */
        start_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        return;
    }

    ESP_LOGI(TAG, "network closed; use the UI button to permit joining");
}

sys_error_t zigbee_radio_open_network(uint8_t seconds)
{
    if (!zigbee_radio_ready) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_INVALID_STATE);
    }
    const ezb_err_t err = ezb_bdb_open_network(seconds);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "network not opened: code=0x%x", (unsigned)err);
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_IO);
    }
    ESP_LOGI(TAG, "network open requested for %u s", (unsigned)seconds);
    return SYS_OK;
}

/* Сеть создана: печатаем её координаты и переходим к steering, который её открывает. */
static void report_network_formed(void)
{
    ezb_extpanid_t extended_pan_id = {0};
    ezb_nwk_get_extended_panid(&extended_pan_id);
    ESP_LOGI(TAG, "network formed: pan=0x%04x ext=0x%08lx%08lx channel=%u short=0x%04x",
             (unsigned)ezb_nwk_get_panid(), (unsigned long)(extended_pan_id.u64 >> 32),
             (unsigned long)(extended_pan_id.u64 & 0xFFFFFFFFu),
             (unsigned)ezb_nwk_get_current_channel(), (unsigned)ezb_nwk_get_short_address());
    start_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
}

/* Устройство ушло: снять незавершённое интервью и попросить сервис убрать записи. */
static bool device_left(ha_device_uid_t uid, uint16_t short_addr)
{
    ESP_LOGI(TAG, "device left: short=0x%04x uid=%llx", (unsigned)short_addr,
             (unsigned long long)uid);
    interview_drop(uid);
    const sys_error_t err = zigbee_submit_leave(uid);
    if (sys_failed(err)) {
        ESP_LOGW(TAG, "leave not queued: uid=%llx err=%u", (unsigned long long)uid,
                 (unsigned)err.code);
        zigbee_diag_record(ZIGBEE_DIAG_TOPOLOGY, err);
    }
    return true;
}

/*
 * Удаление. Устройство нельзя убрать из сети напрямую: держим пометку (Zigbee-private,
 * zigbee_remove_mark) и шлём ZDO Mgmt_Leave, когда оно в сети. Успешный
 * leave кладёт факт leave в сервис — он и удаляет устройство/endpoint'ы, и снимает
 * пометку. Повторный announce/rediscovery помеченного устройства тоже уходит в leave
 * (интервью не запускаем).
 */
#define LEAVE_CTX_MAX 4

typedef struct {
    bool used;
    ha_device_uid_t uid;
} leave_ctx_t;

static leave_ctx_t s_leave_ctx[LEAVE_CTX_MAX];

static leave_ctx_t *leave_ctx_acquire(ha_device_uid_t uid)
{
    leave_ctx_t *free_ctx = NULL;
    for (size_t i = 0; i < LEAVE_CTX_MAX; i++) {
        if (s_leave_ctx[i].used && s_leave_ctx[i].uid == uid) {
            return &s_leave_ctx[i];
        }
        if (!s_leave_ctx[i].used && free_ctx == NULL) {
            free_ctx = &s_leave_ctx[i];
        }
    }
    if (free_ctx != NULL) {
        free_ctx->used = true;
        free_ctx->uid = uid;
    }
    return free_ctx;
}

static void leave_req_done(const ezb_zdo_nwk_mgmt_leave_req_result_t *result, void *user_ctx)
{
    leave_ctx_t *ctx = (leave_ctx_t *)user_ctx;
    const bool ok = result != NULL && result->error == EZB_ERR_NONE && result->rsp != NULL &&
                    result->rsp->status == EZB_ZDP_STATUS_SUCCESS;
    ESP_LOGI(TAG, "leave result: uid=%llx ok=%d", (unsigned long long)(ctx != NULL ? ctx->uid : 0),
             (int)ok);
    if (ok && ctx != NULL) {
        (void)zigbee_submit_leave(ctx->uid);
    }
    if (ctx != NULL) {
        ctx->used = false;
    }
}

sys_error_t zigbee_radio_request_leave(ha_device_uid_t uid)
{
    if (!zigbee_radio_ready) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_INVALID_STATE);
    }
    const ezb_extaddr_t ext = {.u64 = uid};
    ezb_shortaddr_t short_addr = 0;
    if (ezb_address_short_by_extended(&ext, &short_addr) != EZB_ERR_NONE) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_NOT_FOUND);
    }
    leave_ctx_t *ctx = leave_ctx_acquire(uid);
    if (ctx == NULL) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_NO_MEM);
    }
    const ezb_zdo_nwk_mgmt_leave_req_t request = {
        .dst_nwk_addr = short_addr,
        .field = {.device_addr = ext, .remove_children = false, .rejoin = false},
        .cb = leave_req_done,
        .user_ctx = ctx,
    };
    const ezb_err_t err = ezb_zdo_nwk_mgmt_leave_req(&request);
    if (err != EZB_ERR_NONE) {
        ctx->used = false;
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_IO);
    }
    ESP_LOGI(TAG, "leave requested: uid=%llx short=0x%04x", (unsigned long long)uid,
             (unsigned)short_addr);
    return SYS_OK;
}

static bool device_marked_for_removal(ha_device_uid_t uid)
{
    return zigbee_remove_mark_has(uid);
}

bool app_signal_handler(const ezb_app_signal_t *signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        /* Стек поднят и ждёт BDB: без этого шага сеть не создаётся вовсе. */
        start_commissioning(EZB_BDB_MODE_INITIALIZATION);
        return true;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_comm_status_t status =
            *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "device startup status=0x%02x", (unsigned)status);
            return true;
        }
        commission_on_startup();
        return true;
    }

    case EZB_BDB_SIGNAL_FORMATION: {
        const ezb_bdb_comm_status_t status =
            *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "network formation status=0x%02x", (unsigned)status);
            return true;
        }
        report_network_formed();
        return true;
    }

    case EZB_BDB_SIGNAL_STEERING: {
        const ezb_bdb_comm_status_t status =
            *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "network steering done");
        } else {
            ESP_LOGW(TAG, "network steering status=0x%02x", (unsigned)status);
        }
        return true;
    }

    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *annce = ezb_app_signal_get_params(signal);
        const ha_device_uid_t uid = (ha_device_uid_t)annce->device_addr.u64;
        if (device_marked_for_removal(uid)) {
            ESP_LOGI(TAG, "device marked for removal: uid=%llx", (unsigned long long)uid);
            (void)zigbee_radio_request_leave(uid);
            return true;
        }
        ESP_LOGI(TAG, "device joined: short=0x%04x uid=%llx", (unsigned)annce->short_addr,
                 (unsigned long long)uid);
        interview_begin(uid, annce->short_addr);
        return true;
    }

    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        const ezb_zdo_signal_device_update_params_t *update = ezb_app_signal_get_params(signal);
        const ha_device_uid_t uid = (ha_device_uid_t)update->device_addr.u64;
        if (update->status == EZB_ZDO_UPDDEV_DEVICE_LEFT) {
            return device_left(uid, update->short_addr);
        }
        if (device_marked_for_removal(uid)) {
            ESP_LOGI(TAG, "device marked for removal (rejoin): uid=%llx", (unsigned long long)uid);
            (void)zigbee_radio_request_leave(uid);
            return true;
        }
        ESP_LOGI(TAG, "device rejoined: short=0x%04x uid=%llx status=0x%02x",
                 (unsigned)update->short_addr, (unsigned long long)uid, (unsigned)update->status);
        interview_begin(uid, update->short_addr);
        return true;
    }

    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *leave =
            ezb_app_signal_get_params(signal);
        return device_left((ha_device_uid_t)leave->device_addr.u64, leave->short_addr);
    }

    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const uint8_t duration = *(const uint8_t *)ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "network %s for %u s", duration != 0u ? "open" : "closed",
                 (unsigned)duration);
        return true;
    }

    default:
        ESP_LOGI(TAG, "signal %s (0x%04x)", ezb_app_signal_to_string(type), (unsigned)type);
        return false;
    }
}
