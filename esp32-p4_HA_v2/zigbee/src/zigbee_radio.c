#include "zigbee/zigbee_radio.h"

#include <string.h>

#include <stdbool.h>

#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_zigbee.h"
#include "ezbee/af.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/zdo/zdo_dev_srv_disc.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_interview.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/level.h"
#include "ezbee/zcl/cluster/on_off.h"
#include "ezbee/zcl/zcl_core.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "esp_hosted.h"
#include "esp_hosted_openthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ha_model/ha_entities.h"
#include "zigbee/zigbee_diag.h"

/*
 * Порядок подъёма такой же, как в примере esp_hosted zigbee/thermostat:
 * ESP-Hosted по SDIO -> RCP на C6 -> UART со spinel -> стек Zigbee на P4.
 */

static const char *TAG = "zigbee.radio";

#define RADIO_TASK_STACK 8192
#define RADIO_TASK_PRIORITY 5
#define RADIO_START_TIMEOUT_MS 15000
#define COORDINATOR_ENDPOINT 1
#define PRIMARY_CHANNEL_MASK (1l << 15)
#define PERMIT_JOIN_SECONDS 180
#define ZIGBEE_STORAGE_PARTITION "zb_storage"

/* Стек готов принимать команды. Пишет задача радио при старте, читает задача сервиса. */
static volatile bool s_ready;

/*
 * Исход старта асинхронной задачи радио; bootstrap ждёт его на семафоре. Память
 * семафора статическая: задача отдаёт его ровно раз, удалять не нужно.
 */
static StaticSemaphore_t s_radio_init_done_storage;
static SemaphoreHandle_t s_radio_init_done;
static sys_error_t s_radio_init_error;

/*
 * Значение репорта копируется по фактической ширине типа: в записи оно лежит в u32,
 * а знаковые типы должны прийти расширенными по знаку (zigbee_state.c).
 */
static bool report_value(const ezb_zcl_report_attr_variable_t *var, zigbee_report_t *out)
{
    switch (var->attr_type) {
    case HA_ZB_TYPE_BOOL:
    case HA_ZB_TYPE_BITMAP8:
    case HA_ZB_TYPE_UINT8:
    case HA_ZB_TYPE_ENUM8: {
        uint8_t value = 0;
        memcpy(&value, var->attr_value, sizeof(value));
        out->raw = value;
        break;
    }
    case HA_ZB_TYPE_INT8: {
        int8_t value = 0;
        memcpy(&value, var->attr_value, sizeof(value));
        out->raw = (uint32_t)(int32_t)value;
        break;
    }
    case HA_ZB_TYPE_UINT16:
    case HA_ZB_TYPE_ENUM16: {
        uint16_t value = 0;
        memcpy(&value, var->attr_value, sizeof(value));
        out->raw = value;
        break;
    }
    case HA_ZB_TYPE_INT16: {
        int16_t value = 0;
        memcpy(&value, var->attr_value, sizeof(value));
        out->raw = (uint32_t)(int32_t)value;
        break;
    }
    case HA_ZB_TYPE_UINT32:
    case HA_ZB_TYPE_INT32:
    case HA_ZB_TYPE_SINGLE_FLOAT: {
        uint32_t value = 0;
        memcpy(&value, var->attr_value, sizeof(value));
        out->raw = value;
        break;
    }
    default:
        return false; /* тип вне словаря скаляров: факта не будет */
    }

    out->zcl_type = var->attr_type;
    return true;
}

static bool report_from(const ezb_zcl_cmd_hdr_t *header, uint16_t cluster_id,
                        const ezb_zcl_report_attr_variable_t *var, zigbee_report_t *out)
{
    if (header == NULL || var == NULL) {
        return false;
    }

    ezb_extaddr_t uid = {0};
    if (header->src_addr.addr_mode == EZB_ADDR_MODE_EXT) {
        uid = header->src_addr.u.extended_addr;
    } else if (ezb_address_extended_by_short(header->src_addr.u.short_addr, &uid) != EZB_ERR_NONE) {
        return false; /* короткий адрес ещё не разрешён в EUI-64 */
    }

    out->device_uid = (ha_device_uid_t)uid.u64;
    out->endpoint = header->src_ep;
    out->cluster_id = cluster_id;
    out->attr_id = var->attr_id;
    return report_value(var, out);
}

/*
 * Интервью устройства. Пока оно не завершено, запись живёт только здесь, в RAM: в
 * Domain устройство попадает по итогу интервью (docs/services/ZIGBEE.md §9.4).
 */
#define INTERVIEW_DEVICES_MAX 4

typedef struct {
    bool used;
    bool basic_requested; /* Basic читается один раз, с endpoint'а, где он есть */
    ha_device_uid_t uid;
    uint16_t short_addr;
    uint8_t outstanding; /* сколько ответов ещё ждём */
    uint8_t endpoint_count;
    char model[HA_DEVICE_MODEL_MAX];
    zigbee_interview_endpoint_t endpoints[ZIGBEE_INTERVIEW_ENDPOINTS_MAX];
} interview_device_t;

static interview_device_t s_interview[INTERVIEW_DEVICES_MAX];

static interview_device_t *interview_slot(ha_device_uid_t uid, uint16_t short_addr)
{
    interview_device_t *free_slot = NULL;
    for (size_t i = 0; i < INTERVIEW_DEVICES_MAX; i++) {
        interview_device_t *candidate = &s_interview[i];
        if (candidate->used && candidate->uid == uid) {
            return candidate; /* повторный announce: интервью перезапускается */
        }
        if (!candidate->used && free_slot == NULL) {
            free_slot = candidate;
        }
    }
    if (free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        free_slot->uid = uid;
        free_slot->short_addr = short_addr;
    }
    return free_slot;
}

static interview_device_t *interview_by_short(uint16_t short_addr)
{
    for (size_t i = 0; i < INTERVIEW_DEVICES_MAX; i++) {
        if (s_interview[i].used && s_interview[i].short_addr == short_addr) {
            return &s_interview[i];
        }
    }
    return NULL;
}

/* Интервью собрано: отдать его задаче сервиса для записи в Domain (ZIGBEE.md §6). */
static void interview_finish(interview_device_t *device)
{
    zigbee_interview_result_t result = {0};
    result.uid = device->uid;
    memcpy(result.model, device->model, sizeof(result.model));
    result.endpoint_count = device->endpoint_count;
    memcpy(result.endpoints, device->endpoints, sizeof(device->endpoints));

    const sys_error_t queued = zigbee_submit_interview(&result);
    if (sys_failed(queued)) {
        ESP_LOGW(TAG, "interview not queued: uid=%llx err=%u", (unsigned long long)device->uid,
                 (unsigned)queued.code);
        zigbee_diag_record(ZIGBEE_DIAG_TOPOLOGY, queued);
    } else {
        ESP_LOGI(TAG, "interview done: uid=%llx endpoints=%u model=\"%s\"",
                 (unsigned long long)device->uid, (unsigned)device->endpoint_count,
                 device->model);
    }

    device->used = false;
}

static void interview_response(interview_device_t *device)
{
    if (device->outstanding > 0) {
        device->outstanding--;
    }
    if (device->outstanding == 0) {
        interview_finish(device);
    }
}

static bool endpoint_has_basic_server(const zigbee_interview_endpoint_t *endpoint)
{
    for (uint8_t i = 0; i < endpoint->cluster_count; i++) {
        if (endpoint->clusters[i].cluster_id == HA_ZB_CLUSTER_BASIC &&
            endpoint->clusters[i].role == HA_ZB_ROLE_SERVER) {
            return true;
        }
    }
    return false;
}

/* Прочитать Basic: модель — единственное, что интервью добавляет к топологии. */
static bool interview_read_basic(interview_device_t *device, uint8_t endpoint)
{
    uint16_t attributes[] = {EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                             EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID};
    const ezb_zcl_read_attr_cmd_t request = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = device->short_addr,
                .dst_ep = endpoint,
                .src_ep = COORDINATOR_ENDPOINT,
                .cluster_id = EZB_ZCL_CLUSTER_ID_BASIC,
            },
        .payload.attr_number = (uint8_t)(sizeof(attributes) / sizeof(attributes[0])),
        .payload.attr_field = attributes,
    };
    const ezb_err_t err = ezb_zcl_read_attr_cmd_req(&request);
    ESP_LOGI(TAG, "basic read: uid=%llx ep=%u code=0x%x", (unsigned long long)device->uid,
             (unsigned)endpoint, (unsigned)err);
    return err == EZB_ERR_NONE;
}

static void interview_on_simple_desc(const ezb_zdo_simple_desc_req_result_t *result,
                                     void *user_ctx)
{
    interview_device_t *device = (interview_device_t *)user_ctx;
    if (device == NULL) {
        return;
    }

    const ezb_zdp_simple_desc_rsp_field_t *rsp = result->rsp;
    if (result->error == EZB_ERR_NONE && rsp != NULL && rsp->status == EZB_ZDP_STATUS_SUCCESS &&
        device->endpoint_count < ZIGBEE_INTERVIEW_ENDPOINTS_MAX) {
        const ezb_af_simple_desc_t *desc = &rsp->desc;
        const uint16_t *input = desc->app_cluster_list;
        const uint16_t *output = input + desc->app_input_cluster_count;

        zigbee_interview_endpoint_t *endpoint = &device->endpoints[device->endpoint_count];
        uint8_t count = 0;
        const sys_error_t mapped =
            zigbee_clusters_from_lists(input, desc->app_input_cluster_count, output,
                                       desc->app_output_cluster_count, endpoint->clusters,
                                       HA_ENDPOINT_CLUSTERS_MAX, &count);
        if (sys_ok(mapped)) {
            endpoint->endpoint = desc->ep_id;
            endpoint->profile_id = desc->app_profile_id;
            endpoint->device_id = desc->app_device_id;
            endpoint->cluster_count = count;
            device->endpoint_count++;
            ESP_LOGI(TAG, "simple desc: uid=%llx ep=%u clusters=%u",
                     (unsigned long long)device->uid, (unsigned)desc->ep_id, (unsigned)count);
            /* Модель берём с endpoint'а, где Basic действительно есть: иначе ответа нет. */
            if (!device->basic_requested && endpoint_has_basic_server(endpoint) &&
                interview_read_basic(device, desc->ep_id)) {
                device->outstanding++;
                device->basic_requested = true;
            }
        } else {
            ESP_LOGW(TAG, "endpoint %u not stored: uid=%llx code=%u", (unsigned)desc->ep_id,
                     (unsigned long long)device->uid, (unsigned)mapped.code);
            zigbee_diag_record(ZIGBEE_DIAG_TOPOLOGY, mapped);
        }
    }

    interview_response(device);
}

static void interview_on_active_ep(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx)
{
    interview_device_t *device = (interview_device_t *)user_ctx;
    if (device == NULL) {
        return;
    }

    const ezb_zdp_active_ep_rsp_field_t *rsp = result->rsp;
    if (result->error != EZB_ERR_NONE || rsp == NULL || rsp->status != EZB_ZDP_STATUS_SUCCESS ||
        rsp->active_ep_count == 0 || rsp->active_ep_list == NULL) {
        ESP_LOGW(TAG, "active endpoints failed: uid=%llx", (unsigned long long)device->uid);
        device->used = false;
        return;
    }

    const uint8_t wanted = (rsp->active_ep_count > ZIGBEE_INTERVIEW_ENDPOINTS_MAX)
                               ? (uint8_t)ZIGBEE_INTERVIEW_ENDPOINTS_MAX
                               : rsp->active_ep_count;
    ESP_LOGI(TAG, "active endpoints: uid=%llx count=%u", (unsigned long long)device->uid,
             (unsigned)rsp->active_ep_count);
    uint8_t requested = 0;
    for (uint8_t i = 0; i < wanted; i++) {
        const ezb_zdo_simple_desc_req_t request = {
            .dst_nwk_addr = device->short_addr,
            .field = {.nwk_addr_of_interest = device->short_addr,
                      .endpoint = rsp->active_ep_list[i]},
            .cb = interview_on_simple_desc,
            .user_ctx = device,
        };
        if (ezb_zdo_simple_desc_req(&request) == EZB_ERR_NONE) {
            requested++;
        } else {
            ESP_LOGW(TAG, "simple desc not requested: uid=%llx ep=%u",
                     (unsigned long long)device->uid, (unsigned)rsp->active_ep_list[i]);
        }
    }

    if (requested == 0) {
        device->used = false;
        return;
    }
    device->outstanding = requested;
}

static void interview_on_read_attr(const ezb_zcl_cmd_read_attr_rsp_message_t *message)
{
    if (message == NULL || message->in.header == NULL ||
        message->info.cluster_id != EZB_ZCL_CLUSTER_ID_BASIC ||
        message->in.header->src_addr.addr_mode != EZB_ADDR_MODE_SHORT) {
        return;
    }

    interview_device_t *device = interview_by_short(message->in.header->src_addr.u.short_addr);
    if (device == NULL) {
        ESP_LOGI(TAG, "basic read rsp for unknown short=0x%04x",
                 (unsigned)message->in.header->src_addr.u.short_addr);
        return;
    }
    ESP_LOGI(TAG, "basic read rsp: uid=%llx status=0x%02x", (unsigned long long)device->uid,
             (unsigned)message->info.status);

    for (const ezb_zcl_read_attr_rsp_variable_t *var = message->in.variables; var != NULL;
         var = var->next) {
        if (var->status != EZB_ZCL_STATUS_SUCCESS ||
            var->attr_id != EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID || var->attr_value == NULL) {
            continue;
        }
        const uint8_t length = *(const uint8_t *)var->attr_value;
        const char *text = (const char *)var->attr_value + 1;
        const size_t room = sizeof(device->model) - 1;
        const size_t copied = (length < room) ? length : room;
        memcpy(device->model, text, copied);
        device->model[copied] = '\0';
    }

    interview_response(device);
}

/* Устройство появилось в сети (announce или rejoin): запускаем интервью. */
static void interview_begin(ha_device_uid_t uid, uint16_t short_addr)
{
    interview_device_t *device = interview_slot(uid, short_addr);
    if (device == NULL) {
        ESP_LOGW(TAG, "interview table full: uid=%llx", (unsigned long long)uid);
        return;
    }
    if (device->outstanding > 0) {
        return; /* интервью уже идёт: повторный announce/rejoin его не перезапускает */
    }

    device->endpoint_count = 0;
    device->model[0] = '\0';
    device->basic_requested = false;
    device->outstanding = 1;

    const ezb_zdo_active_ep_req_t request = {
        .dst_nwk_addr = short_addr,
        .field = {.nwk_addr_of_interest = short_addr},
        .cb = interview_on_active_ep,
        .user_ctx = device,
    };
    if (ezb_zdo_active_ep_req(&request) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "active endpoints not requested: uid=%llx", (unsigned long long)uid);
        device->used = false;
    }
}

static void core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (message == NULL) {
        return;
    }

    if (callback_id == EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID) {
        interview_on_read_attr((const ezb_zcl_cmd_read_attr_rsp_message_t *)message);
        return;
    }

    if (callback_id != EZB_ZCL_CORE_REPORT_ATTR_CB_ID) {
        return;
    }

    const ezb_zcl_cmd_report_attr_message_t *report = message;
    for (const ezb_zcl_report_attr_variable_t *var = report->in.variables; var != NULL;
         var = var->next) {
        zigbee_report_t zcl_report = {0};
        if (!report_from(report->in.header, report->info.cluster_id, var, &zcl_report)) {
            continue;
        }

        const sys_error_t err = zigbee_submit_report(&zcl_report);
        if (sys_failed(err)) {
            zigbee_diag_record(ZIGBEE_DIAG_REPORT_QUEUE, err);
        }
    }
}

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
 * уже связанный — открывает на PERMIT_JOIN_SECONDS, чтобы устройства могли подключиться.
 */
static void commission_on_startup(void)
{
    if (ezb_bdb_is_factory_new()) {
        start_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        return;
    }

    const ezb_err_t open = ezb_bdb_open_network(PERMIT_JOIN_SECONDS);
    if (open != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "network not opened: code=0x%x", (unsigned)open);
    }
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

static bool app_signal_handler(const ezb_app_signal_t *signal)
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
        ESP_LOGI(TAG, "device joined: short=0x%04x uid=%llx", (unsigned)annce->short_addr,
                 (unsigned long long)annce->device_addr.u64);
        interview_begin((ha_device_uid_t)annce->device_addr.u64, annce->short_addr);
        return true;
    }

    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        const ezb_zdo_signal_device_update_params_t *update = ezb_app_signal_get_params(signal);
        if (update->status == EZB_ZDO_UPDDEV_DEVICE_LEFT) {
            ESP_LOGI(TAG, "device left: short=0x%04x", (unsigned)update->short_addr);
            return true;
        }
        ESP_LOGI(TAG, "device rejoined: short=0x%04x uid=%llx status=0x%02x",
                 (unsigned)update->short_addr, (unsigned long long)update->device_addr.u64,
                 (unsigned)update->status);
        interview_begin((ha_device_uid_t)update->device_addr.u64, update->short_addr);
        return true;
    }

    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *leave =
            ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "device left: short=0x%04x", (unsigned)leave->short_addr);
        return true;
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

/* Сбой подъёма радио — ошибка слоя Zigbee; задача сообщает её bootstrap'у.
 * code — сырой код SDK для диагностики; 0, когда исходного кода нет. */
static sys_error_t radio_fail(const char *what, uint32_t code)
{
    if (code != 0u) {
        ESP_LOGE(TAG, "%s: code=0x%x", what, (unsigned)code);
    } else {
        ESP_LOGE(TAG, "%s", what);
    }
    return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_IO);
}

static sys_error_t create_coordinator_device(void)
{
    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    ezb_af_ep_config_t ep_config = {0};
    ep_config.ep_id = COORDINATOR_ENDPOINT;
    ep_config.app_profile_id = HA_ZB_PROFILE_HA;
    ep_config.app_device_id = 0x0005u;

    ezb_af_ep_desc_t endpoint = ezb_af_create_gateway_endpoint(&ep_config);
    const ezb_err_t add = ezb_af_device_add_endpoint_desc(device, endpoint);
    if (add != EZB_ERR_NONE) {
        return radio_fail("add coordinator endpoint failed", (uint32_t)add);
    }
    const ezb_err_t registered = ezb_af_device_desc_register(device);
    if (registered != EZB_ERR_NONE) {
        return radio_fail("register coordinator device failed", (uint32_t)registered);
    }
    ezb_zcl_core_action_handler_register(core_action_handler);
    return SYS_OK;
}

static void radio_uart_config(esp_zigbee_config_t *config,
                              const esp_hosted_openthread_radio_config_t *radio)
{
    config->platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_UART_RCP;

    const esp_hosted_openthread_uart_config_t *src = &radio->radio_uart_config;
    esp_zigbee_uart_config_t *dst = &config->platform_config.radio_config.radio_uart_config;
    dst->port = src->port;
    dst->uart_config.baud_rate = src->baud_rate;
    dst->uart_config.data_bits = src->data_bits;
    dst->uart_config.parity = src->parity;
    dst->uart_config.stop_bits = src->stop_bits;
    dst->uart_config.flow_ctrl = src->flow_ctrl;
    dst->uart_config.rx_flow_ctrl_thresh = src->rx_flow_ctrl_thresh;
    dst->uart_config.source_clk = src->source_clk;
    dst->rx_pin = src->rx_pin;
    dst->tx_pin = src->tx_pin;
}

/*
 * Настройка и старт стека Zigbee. Возвращает ошибку, а не завершает задачу:
 * решение «система без Zigbee не поднимается» принимает bootstrap (app_main).
 */
static sys_error_t init_zigbee_stack(void)
{
    esp_hosted_openthread_radio_config_t radio = {0};
    const int radio_ok = esp_hosted_openthread_get_radio_config(&radio);
    if (radio_ok != 0 || radio.type != HOSTED_OPENTHREAD_TRANSPORT_UART) {
        return radio_fail("expected UART spinel transport to RCP", (uint32_t)radio_ok);
    }

    esp_zigbee_config_t config = {
        .device_config =
            {
                .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,
                .install_code_policy = false,
                .zczr_config = { .max_children = 10 },
            },
        .platform_config = { .storage_partition_name = ZIGBEE_STORAGE_PARTITION },
    };
    radio_uart_config(&config, &radio);

    const esp_err_t inited = esp_zigbee_init(&config);
    if (inited != ESP_OK) {
        return radio_fail("esp_zigbee_init failed", (uint32_t)inited);
    }

    ezb_aps_secur_enable_distributed_security(false);
    const ezb_err_t channel = ezb_bdb_set_primary_channel_set(PRIMARY_CHANNEL_MASK);
    if (channel != EZB_ERR_NONE) {
        return radio_fail("set primary channel set failed", (uint32_t)channel);
    }
    const ezb_err_t handler = ezb_app_signal_add_handler(app_signal_handler);
    if (handler != EZB_ERR_NONE) {
        return radio_fail("add app signal handler failed", (uint32_t)handler);
    }

    const sys_error_t device = create_coordinator_device();
    if (sys_failed(device)) {
        return device;
    }

    const esp_err_t started = esp_zigbee_start(false);
    if (started != ESP_OK) {
        return radio_fail("esp_zigbee_start failed", (uint32_t)started);
    }
    return SYS_OK;
}

static sys_error_t radio_error(ezb_err_t result)
{
    if (result == EZB_ERR_NONE) {
        return SYS_OK;
    }
    return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_IO);
}

/* Адресат команды задаётся EUI-64: short address разрешается стеком под капотом. */
static ezb_zcl_cluster_cmd_ctrl_t command_control(const ha_zb_command_t *command)
{
    const ezb_extaddr_t eui = {.u64 = command->device_uid};
    const ezb_zcl_cluster_cmd_ctrl_t control = {
        .dst_addr = EZB_ADDRESS_EXTENDED(eui),
        .dst_ep = command->dst_endpoint,
        .src_ep = COORDINATOR_ENDPOINT,
        .dis_default_rsp = false,
    };
    return control;
}

static sys_error_t send_on_off(const ha_zb_command_t *command)
{
    const ezb_zcl_on_off_cmd_t request = {.cmd_ctrl = command_control(command)};

    switch (command->command_id) {
    case HA_ZB_CMD_ON_OFF_OFF: return radio_error(ezb_zcl_on_off_off_cmd_req(&request));
    case HA_ZB_CMD_ON_OFF_ON: return radio_error(ezb_zcl_on_off_on_cmd_req(&request));
    case HA_ZB_CMD_ON_OFF_TOGGLE: return radio_error(ezb_zcl_on_off_toggle_cmd_req(&request));
    default: return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_ARG);
    }
}

/* Аргумент ровно один — целевой уровень 0..254; время перехода берётся из OnOff. */
static sys_error_t send_level_move_to_level(const ha_zb_command_t *command)
{
    if (command->command_id != HA_ZB_CMD_LEVEL_MOVE_TO_LEVEL || command->args_len != 1) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_ARG);
    }

    const ezb_zcl_level_move_to_level_cmd_t request = {
        .cmd_ctrl = command_control(command),
        .payload = {.level = command->args[0], .transition_time = 0xFFFF},
    };
    return radio_error(ezb_zcl_level_move_to_level_cmd_req(&request));
}

sys_error_t zigbee_radio_send(const ha_zb_command_t *command)
{
    if (command == NULL) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_ARG);
    }
    if (!s_ready) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_STATE);
    }

    switch (command->cluster_id) {
    case HA_ZB_CLUSTER_ON_OFF: return send_on_off(command);
    case HA_ZB_CLUSTER_LEVEL_CONTROL: return send_level_move_to_level(command);
    default: return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_ARG);
    }
}

/* Подъём RCP на C6: ESP-Hosted по SDIO, затем spinel-транспорт. */
static sys_error_t bring_up_coproc_radio(void)
{
    const esp_err_t hosted = esp_hosted_init();
    if (hosted != ESP_OK) {
        return radio_fail("esp_hosted_init failed", (uint32_t)hosted);
    }
    const esp_err_t connected = esp_hosted_connect_to_slave();
    if (connected != ESP_OK) {
        return radio_fail("slave over SDIO not connected", (uint32_t)connected);
    }
    /* Пример esp_hosted делает то же в esp_hosted_openthread_app_init(); заголовок того
     * вспомогательного файла в компонент не входит, поэтому шаги расписаны здесь. */
    if (esp_hosted_openthread_rcp_query(HOSTED_OPENTHREAD_QUERY_CONFIGURED) != ESP_OK ||
        esp_hosted_openthread_rcp_init() != ESP_OK ||
        esp_hosted_openthread_rcp_query(HOSTED_OPENTHREAD_QUERY_INITED) != ESP_OK ||
        esp_hosted_openthread_rcp_start() != ESP_OK ||
        esp_hosted_openthread_rcp_query(HOSTED_OPENTHREAD_QUERY_ENABLED) != ESP_OK ||
        esp_hosted_openthread_rcp_query(HOSTED_OPENTHREAD_QUERY_READY) != ESP_OK) {
        return radio_fail("RCP on the co-processor not started", 0);
    }
    return SYS_OK;
}

/*
 * NVS: при исчерпании страниц или смене версии IDF раздел стирается и поднимается
 * заново; иначе первый доступ к NVS отдаст ошибку позже и размыто.
 */
static sys_error_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        const esp_err_t erased = nvs_flash_erase();
        if (erased != ESP_OK) {
            return radio_fail("nvs erase failed", (uint32_t)erased);
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return radio_fail("nvs init failed", (uint32_t)err);
    }

    err = nvs_flash_init_partition(ZIGBEE_STORAGE_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        const esp_err_t erased = nvs_flash_erase_partition(ZIGBEE_STORAGE_PARTITION);
        if (erased != ESP_OK) {
            return radio_fail("nvs zb_storage erase failed", (uint32_t)erased);
        }
        err = nvs_flash_init_partition(ZIGBEE_STORAGE_PARTITION);
    }
    if (err != ESP_OK) {
        return radio_fail("nvs zb_storage init failed", (uint32_t)err);
    }
    return SYS_OK;
}

/*
 * Задача радио: поднимает RCP и стек, сообщает исход bootstrap'у и, при успехе,
 * уходит в mainloop. Провал не завершает систему через abort: app_main решает,
 * продолжать ли без Zigbee (без него система бессмысленна — services/ZIGBEE.md §5).
 */
static void zigbee_stack_task(void *arg)
{
    (void)arg;

    sys_error_t err = init_nvs();
    if (sys_ok(err)) {
        err = bring_up_coproc_radio();
    }
    if (sys_ok(err)) {
        err = init_zigbee_stack();
    }

    s_radio_init_error = err;
    if (sys_ok(err)) {
        s_ready = true;
    }
    xSemaphoreGive(s_radio_init_done);

    if (sys_failed(err)) {
        vTaskDelete(NULL);
        return;
    }

    esp_zigbee_launch_mainloop();
    vTaskDelete(NULL);
}

sys_error_t zigbee_radio_start(domain_t *domain)
{
    if (domain == NULL) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_ARG);
    }

    s_radio_init_done = xSemaphoreCreateBinaryStatic(&s_radio_init_done_storage);
    if (s_radio_init_done == NULL) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_NO_MEM);
    }

    if (xTaskCreate(zigbee_stack_task, "zigbee_radio", RADIO_TASK_STACK, NULL, RADIO_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_NO_MEM);
    }

    /*
     * Без Zigbee система не поднимается: исход старта должен дойти до bootstrap'а, а не
     * потеряться в отсоединённой задаче. Таймаут страхует от зависшего подъёма: задачу
     * мы не отменяем, поэтому она может отработать позже, но решение уже принято
     * (services/ZIGBEE.md §5).
     */
    if (xSemaphoreTake(s_radio_init_done, pdMS_TO_TICKS(RADIO_START_TIMEOUT_MS)) != pdTRUE) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_IO);
    }
    return s_radio_init_error;
}
