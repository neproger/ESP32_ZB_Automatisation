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
#include "ezbee/zdo/zdo_nwk_mgmt.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_binding.h"
#include "zigbee/zigbee_interview.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/level.h"
#include "ezbee/zcl/cluster/on_off.h"
#include "ezbee/zcl/zcl_core.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ha_model/ha_entities.h"
#include "zigbee/zigbee_diag.h"

/*
 * Радио: стек Zigbee на P4, 802.15.4-радио — RCP на C6 (прошивка ot_rcp,
 * standalone spinel). Связь P4<->C6 — только UART; esp_hosted для Zigbee не нужен
 * (он занят Wi-Fi на отдельном сопроцессоре).
 */

static const char *TAG = "zigbee.radio";

#define RADIO_TASK_STACK 8192
#define RADIO_TASK_PRIORITY 5
#define RADIO_START_TIMEOUT_MS 15000
#define COORDINATOR_ENDPOINT 1
#define PRIMARY_CHANNEL_MASK (1l << 15)
#define PERMIT_JOIN_SECONDS 180
#define ZIGBEE_STORAGE_PARTITION "zb_storage"

/* Spinel-UART к ot_rcp на C6: пины P4 GPIO29(TX)/30(RX) через JP1. */
#define RCP_UART_PORT UART_NUM_1
#define RCP_UART_TX_PIN 29
#define RCP_UART_RX_PIN 30
#define RCP_UART_BAUD 460800
#define RCP_START_DELAY_MS 1000

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

/* EUI-64 источника кадра: расширенный адрес как есть, короткий — через таблицу стека. */
static bool device_uid_from(const ezb_address_t *addr, ha_device_uid_t *out)
{
    if (addr->addr_mode == EZB_ADDR_MODE_EXT) {
        *out = (ha_device_uid_t)addr->u.extended_addr.u64;
        return true;
    }
    ezb_extaddr_t uid = {0};
    if (addr->addr_mode == EZB_ADDR_MODE_SHORT &&
        ezb_address_extended_by_short(addr->u.short_addr, &uid) == EZB_ERR_NONE) {
        *out = (ha_device_uid_t)uid.u64;
        return true;
    }
    return false; /* адрес ещё не разрешён в EUI-64 */
}

static bool report_from(const ezb_zcl_cmd_hdr_t *header, uint16_t cluster_id,
                        const ezb_zcl_report_attr_variable_t *var, zigbee_report_t *out)
{
    if (header == NULL || var == NULL) {
        return false;
    }
    if (!device_uid_from(&header->src_addr, &out->device_uid)) {
        return false;
    }
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

/* Устройство ушло: незавершённое интервью больше не нужно. */
static void interview_drop(ha_device_uid_t uid)
{
    for (size_t i = 0; i < INTERVIEW_DEVICES_MAX; i++) {
        if (s_interview[i].used && s_interview[i].uid == uid) {
            s_interview[i].used = false;
        }
    }
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

    /* Без binding и reporting устройство не шлёт состояние координатору. */
    zigbee_binding_apply(device->uid, device->short_addr, device->endpoints,
                         device->endpoint_count);

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

    if (callback_id == EZB_ZCL_CORE_CONFIG_REPORT_RSP_CB_ID) {
        const ezb_zcl_cmd_config_report_rsp_message_t *rsp = message;
        for (const ezb_zcl_config_report_rsp_variable_t *var = rsp->in.variables; var != NULL;
             var = var->next) {
            if (var->status != EZB_ZCL_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "reporting config rsp: cluster=%04x attr=%04x status=0x%02x",
                         (unsigned)rsp->info.cluster_id, (unsigned)var->attr_id,
                         (unsigned)var->status);
            }
        }
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

/*
 * События от устройств: входящие cluster-specific команды (кнопка/пульт). У обычных
 * OnOff On/Off/Toggle в SDK нет отдельного колбэка, поэтому берём сырой ZCL-кадр,
 * отбрасываем foundation-команды и ответы и публикуем EVENT.
 */
static bool raw_frame_handler(const ezb_zcl_raw_frame_t *raw)
{
    const ezb_zcl_cmd_hdr_t *header = raw != NULL ? raw->header : NULL;
    if (header == NULL || header->profile_id != HA_ZB_PROFILE_HA) {
        return false;
    }
    if (EZB_ZCL_CMD_FC_GET_FRAME_TYPE(header->fc) != EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC ||
        EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(header->fc)) {
        return false; /* не foundation-команда и не ответ: клиент шлёт команду серверу */
    }

    zigbee_event_t event = {0};
    if (!device_uid_from(&header->src_addr, &event.device_uid)) {
        return false;
    }
    event.cluster_id = header->cluster_id;
    event.command_id = header->cmd_id;
    event.endpoint = header->src_ep;

    const uint16_t length = (raw->payload_length > ZIGBEE_EVENT_PAYLOAD_MAX)
                                ? (uint16_t)ZIGBEE_EVENT_PAYLOAD_MAX
                                : raw->payload_length;
    event.payload_length = (uint8_t)length;
    if (length > 0 && raw->payload != NULL) {
        memcpy(event.payload, raw->payload, length);
    }

    const sys_error_t err = zigbee_submit_event(&event);
    if (sys_failed(err)) {
        ESP_LOGW(TAG, "event not queued: uid=%llx cluster=%04x err=%u",
                 (unsigned long long)event.device_uid, (unsigned)event.cluster_id,
                 (unsigned)err.code);
        zigbee_diag_record(ZIGBEE_DIAG_EVENT, err);
    }
    return false; /* кадр не наш: пусть стек обработает его сам */
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
        /* Первый запуск: сеть нужно создать, formation открывает её для steering. */
        start_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        return;
    }

    /* Сеть уже создана: на старте её не открываем — подключение новых устройств только
     * по команде из UI (zigbee_radio_open_network). */
    ESP_LOGI(TAG, "network closed; use the UI button to permit joining");
}

sys_error_t zigbee_radio_open_network(uint8_t seconds)
{
    if (!s_ready) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_STATE);
    }
    const ezb_err_t err = ezb_bdb_open_network(seconds);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "network not opened: code=0x%x", (unsigned)err);
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_IO);
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
 * Удаление. Устройство нельзя убрать из сети напрямую: держим пометку (список в
 * Domain, HA_ENTITY_DEVICE_REMOVE) и шлём ZDO Mgmt_Leave, когда оно в сети. Успешный
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
static domain_t *s_radio_domain;

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
    if (!s_ready) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_STATE);
    }
    const ezb_extaddr_t ext = {.u64 = uid};
    ezb_shortaddr_t short_addr = 0;
    if (ezb_address_short_by_extended(&ext, &short_addr) != EZB_ERR_NONE) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_NOT_FOUND);
    }
    leave_ctx_t *ctx = leave_ctx_acquire(uid);
    if (ctx == NULL) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_NO_MEM);
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
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_IO);
    }
    ESP_LOGI(TAG, "leave requested: uid=%llx short=0x%04x", (unsigned long long)uid,
             (unsigned)short_addr);
    return SYS_OK;
}

static bool device_marked_for_removal(ha_device_uid_t uid)
{
    if (s_radio_domain == NULL) {
        return false;
    }
    ha_device_remove_record_t record = {0};
    return sys_ok(domain_entity_get(s_radio_domain, (domain_entity_t)HA_ENTITY_DEVICE_REMOVE, &uid,
                                    &record));
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
    /* Принять репорты и отправить команды координатор может только через client-кластер. */
    zigbee_binding_add_client_clusters(endpoint);

    const ezb_err_t add = ezb_af_device_add_endpoint_desc(device, endpoint);
    if (add != EZB_ERR_NONE) {
        return radio_fail("add coordinator endpoint failed", (uint32_t)add);
    }
    const ezb_err_t registered = ezb_af_device_desc_register(device);
    if (registered != EZB_ERR_NONE) {
        return radio_fail("register coordinator device failed", (uint32_t)registered);
    }
    ezb_zcl_core_action_handler_register(core_action_handler);
    ezb_zcl_raw_command_handler_register(raw_frame_handler);
    return SYS_OK;
}

/*
 * Spinel-UART к C6 с прошивкой ot_rcp (standalone RCP, без esp_hosted). Пины P4
 * GPIO29(TX)/GPIO30(RX) через JP1 — те же, что раньше для spinel.
 */
static void radio_uart_config(esp_zigbee_config_t *config)
{
    config->platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_UART_RCP;

    esp_zigbee_uart_config_t *dst = &config->platform_config.radio_config.radio_uart_config;
    dst->port = RCP_UART_PORT;
    dst->uart_config.baud_rate = RCP_UART_BAUD;
    dst->uart_config.data_bits = UART_DATA_8_BITS;
    dst->uart_config.parity = UART_PARITY_DISABLE;
    dst->uart_config.stop_bits = UART_STOP_BITS_1;
    dst->uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    dst->uart_config.rx_flow_ctrl_thresh = 0;
    dst->uart_config.source_clk = UART_SCLK_DEFAULT;
    dst->rx_pin = (gpio_num_t)RCP_UART_RX_PIN;
    dst->tx_pin = (gpio_num_t)RCP_UART_TX_PIN;
}

/*
 * Настройка и старт стека Zigbee. Возвращает ошибку, а не завершает задачу:
 * решение «система без Zigbee не поднимается» принимает bootstrap (app_main).
 */
static sys_error_t init_zigbee_stack(void)
{
    esp_zigbee_config_t config = {
        .device_config =
            {
                .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,
                .install_code_policy = false,
                .zczr_config = { .max_children = 10 },
            },
        .platform_config = { .storage_partition_name = ZIGBEE_STORAGE_PARTITION },
    };
    radio_uart_config(&config);

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

/*
 * Прошивка ot_rcp на C6 стартует RCP самостоятельно; P4 только соединяется по UART.
 * Ждём загрузку C6 (иначе первые spinel-кадры уйдут в никуда).
 */
static sys_error_t bring_up_coproc_radio(void)
{
    vTaskDelay(pdMS_TO_TICKS(RCP_START_DELAY_MS));
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
    s_radio_domain = domain;

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
