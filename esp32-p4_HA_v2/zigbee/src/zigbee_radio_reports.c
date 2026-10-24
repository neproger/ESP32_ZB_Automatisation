#include "zigbee_radio_internal.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "ezbee/af.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "ezbee/zdo/zdo_dev_srv_disc.h"
#include "zigbee/zigbee_adapter_ids.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_profiles.h"
#include "zigbee/zigbee_binding.h"
#include "zigbee/zigbee_device_store.h"
#include "zigbee/zigbee_diag.h"
#include "zigbee/zigbee_interview.h"

/* Декод ZCL-репортов, интервью устройства и приём report/event-кадров. */

static const char *TAG = "zigbee.radio";

static bool device_uid_from(const ezb_address_t *addr, ha_device_uid_t *out);

/* Скаляр ZCL → raw u32 (в записи значение лежит в u32; знаковые — расширены по знаку). */
static bool zcl_value_to_raw(uint8_t attr_type, const void *value, uint32_t *out_raw)
{
    if (value == NULL) {
        return false;
    }
    switch (attr_type) {
    case HA_ZB_TYPE_BOOL:
    case HA_ZB_TYPE_BITMAP8:
    case HA_ZB_TYPE_UINT8:
    case HA_ZB_TYPE_ENUM8: {
        uint8_t v = 0;
        memcpy(&v, value, sizeof(v));
        *out_raw = v;
        break;
    }
    case HA_ZB_TYPE_INT8: {
        int8_t v = 0;
        memcpy(&v, value, sizeof(v));
        *out_raw = (uint32_t)(int32_t)v;
        break;
    }
    case HA_ZB_TYPE_UINT16:
    case HA_ZB_TYPE_ENUM16: {
        uint16_t v = 0;
        memcpy(&v, value, sizeof(v));
        *out_raw = v;
        break;
    }
    case HA_ZB_TYPE_INT16: {
        int16_t v = 0;
        memcpy(&v, value, sizeof(v));
        *out_raw = (uint32_t)(int32_t)v;
        break;
    }
    case HA_ZB_TYPE_UINT32:
    case HA_ZB_TYPE_INT32:
    case HA_ZB_TYPE_SINGLE_FLOAT: {
        uint32_t v = 0;
        memcpy(&v, value, sizeof(v));
        *out_raw = v;
        break;
    }
    default:
        return false; /* тип вне словаря скаляров: факта не будет */
    }
    return true;
}

static bool report_value(const ezb_zcl_report_attr_variable_t *var, zigbee_report_t *out)
{
    if (!zcl_value_to_raw(var->attr_type, var->attr_value, &out->raw)) {
        return false;
    }
    out->zcl_type = var->attr_type;
    return true;
}

/*
 * Ответ на Read Attributes для состояния (после интервью): превращаем в репорты, чтобы
 * состояние появилось сразу, не дожидаясь первого change/periodic репорта.
 */
static void state_read_on_attr_rsp(const ezb_zcl_cmd_read_attr_rsp_message_t *message)
{
    if (message == NULL || message->in.header == NULL ||
        message->in.header->src_addr.addr_mode != EZB_ADDR_MODE_SHORT) {
        return;
    }
    for (const ezb_zcl_read_attr_rsp_variable_t *var = message->in.variables; var != NULL;
         var = var->next) {
        if (var->status != EZB_ZCL_STATUS_SUCCESS || var->attr_value == NULL) {
            continue;
        }
        zigbee_report_t rep = {0};
        if (!device_uid_from(&message->in.header->src_addr, &rep.device_uid) ||
            !zcl_value_to_raw(var->attr_type, var->attr_value, &rep.raw)) {
            continue;
        }
        rep.endpoint = message->in.header->src_ep;
        rep.cluster_id = message->info.cluster_id;
        rep.attr_id = var->attr_id;
        rep.zcl_type = var->attr_type;
        (void)zigbee_submit_report(&rep);
    }
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
    char manufacturer[HA_DEVICE_NAME_MAX];
    char model[HA_DEVICE_MODEL_MAX];
    zigbee_interview_endpoint_t endpoints[ZIGBEE_INTERVIEW_ENDPOINTS_MAX];
} interview_device_t;

static interview_device_t s_interview[INTERVIEW_DEVICES_MAX];

/*
 * Устройства, которым в этой загрузке уже настроены binding/reporting. Host применяет
 * Configure Reporting только при интервью, а после перезагрузки ХОСТА устройство не
 * объявляется заново — поэтому при первом его репорте перезапускаем интервью, чтобы
 * новые настройки (напр. min_interval=0) вступили в силу без перезапуска устройства.
 */
#define RADIO_CONFIGURED_MAX 16
static ha_device_uid_t s_configured_uid[RADIO_CONFIGURED_MAX];

static bool configured_seen(ha_device_uid_t uid)
{
    for (size_t i = 0; i < RADIO_CONFIGURED_MAX; i++) {
        if (s_configured_uid[i] == uid) {
            return true;
        }
    }
    return false;
}

static void configured_add(ha_device_uid_t uid)
{
    if (configured_seen(uid)) {
        return;
    }
    for (size_t i = 0; i < RADIO_CONFIGURED_MAX; i++) {
        if (s_configured_uid[i] == 0) {
            s_configured_uid[i] = uid;
            return;
        }
    }
}

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
void interview_drop(ha_device_uid_t uid)
{
    for (size_t i = 0; i < INTERVIEW_DEVICES_MAX; i++) {
        if (s_interview[i].used && s_interview[i].uid == uid) {
            s_interview[i].used = false;
        }
    }
}

/* Прочитать текущие значения reportable-атрибутов (после подписки), чтобы состояние
 * появилось сразу, а не ждало change/periodic репорта. */
static void interview_read_states(const interview_device_t *device, uint8_t only_endpoint)
{
    for (uint8_t e = 0; e < device->endpoint_count; e++) {
        const zigbee_interview_endpoint_t *ep = &device->endpoints[e];
        if (only_endpoint != 0 && ep->endpoint != only_endpoint) {
            continue;
        }
        for (uint8_t c = 0; c < ep->cluster_count; c++) {
            if (ep->clusters[c].role != HA_ZB_ROLE_SERVER) {
                continue;
            }
            const uint16_t cluster_id = ep->clusters[c].cluster_id;
            uint16_t attrs[16] = {0};
            const uint8_t n = zigbee_binding_attrs_for(cluster_id, attrs, 16);
            if (n == 0) {
                continue;
            }
            const ezb_zcl_read_attr_cmd_t request = {
                .cmd_ctrl =
                    {
                        .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                        .dst_addr.u.short_addr = device->short_addr,
                        .dst_ep = ep->endpoint,
                        .src_ep = COORDINATOR_ENDPOINT,
                        .cluster_id = cluster_id,
                    },
                .payload.attr_number = n,
                .payload.attr_field = attrs,
            };
            (void)ezb_zcl_read_attr_cmd_req(&request);
        }
    }
}

/* Интервью собрано: отдать его задаче сервиса для записи в Domain (ZIGBEE.md §6). */
static void interview_finish(interview_device_t *device)
{
    if (device->endpoint_count == 0) {
        /*
         * Интервью не собралось (устройство не ответило на ZDO). Пустой Device не создаём —
         * переинтервьюируем при следующем announce/rejoin.
         */
        ESP_LOGW(TAG, "interview empty (no endpoints): uid=%llx", (unsigned long long)device->uid);
        zigbee_diag_record(ZIGBEE_DIAG_TOPOLOGY,
                           sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_NOT_FOUND));
        device->used = false;
        return;
    }

    zigbee_interview_result_t result = {0};
    result.uid = device->uid;
    memcpy(result.manufacturer, device->manufacturer, sizeof(result.manufacturer));
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

    /* Каталог: если у профиля задан один endpoint (кнопка), конфигурируем только его.
     * Шквал secure-bind'ов на фантомные endpoint'ы переполняет очередь устройства. */
    const zb_device_signature_t signature = {.manufacturer = device->manufacturer,
                                             .model = device->model};
    const zb_device_profile_t *profile = NULL;
    const zb_profile_match_t match = zb_profiles_match(&signature, &profile);
    const uint8_t only_endpoint =
        (match != ZB_PROFILE_NONE && profile->capability != NULL)
            ? profile->capability->button_endpoint
            : 0;
    const zigbee_interview_endpoint_t *only = NULL;
    if (only_endpoint != 0) {
        for (uint8_t i = 0; i < device->endpoint_count; i++) {
            if (device->endpoints[i].endpoint == only_endpoint) {
                only = &device->endpoints[i];
                break;
            }
        }
    }

    /* Без binding и reporting устройство не шлёт состояние координатору. */
    if (only != NULL) {
        zigbee_binding_apply(device->uid, device->short_addr, only, 1);
    } else {
        zigbee_binding_apply(device->uid, device->short_addr, device->endpoints,
                             device->endpoint_count);
    }
    interview_read_states(device, only_endpoint);
    configured_add(device->uid); /* reporting настроен в этой загрузке */

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
    if (result->error != EZB_ERR_NONE || rsp == NULL || rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "simple desc failed: uid=%llx err=%d status=0x%02x",
                 (unsigned long long)device->uid, (int)result->error,
                 (unsigned)(rsp != NULL ? rsp->status : 0));
    }
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
            ESP_LOGI(TAG, "simple desc: uid=%llx ep=%u in=%u out=%u clusters=%u",
                     (unsigned long long)device->uid, (unsigned)desc->ep_id,
                     (unsigned)desc->app_input_cluster_count,
                     (unsigned)desc->app_output_cluster_count, (unsigned)count);
            for (uint8_t ci = 0; ci < endpoint->cluster_count; ci++) {
                ESP_LOGI(TAG, "  ep=%u cluster=0x%04x role=%s", (unsigned)desc->ep_id,
                         (unsigned)endpoint->clusters[ci].cluster_id,
                         endpoint->clusters[ci].role == HA_ZB_ROLE_SERVER ? "server" : "client");
            }
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
        if (var->status != EZB_ZCL_STATUS_SUCCESS || var->attr_value == NULL) {
            continue;
        }
        char *target = NULL;
        size_t target_size = 0;
        if (var->attr_id == EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID) {
            target = device->model;
            target_size = sizeof(device->model);
        } else if (var->attr_id == EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID) {
            target = device->manufacturer;
            target_size = sizeof(device->manufacturer);
        }
        if (target == NULL) {
            continue;
        }
        const uint8_t length = *(const uint8_t *)var->attr_value;
        const char *text = (const char *)var->attr_value + 1;
        const size_t room = target_size - 1;
        const size_t copied = (length < room) ? length : room;
        memcpy(target, text, copied);
        target[copied] = '\0';
    }

    interview_response(device);
}

/* Устройство появилось в сети (announce или rejoin): запускаем интервью. */
void interview_begin(ha_device_uid_t uid, uint16_t short_addr)
{
    /* Устройство уже известно адаптеру (восстановлено из флеша) — топология есть,
     * интервью не требуется. Устройство шлёт репорты по сохранённой на его стороне
     * конфигурации; состояние прилетит как обычно. */
    if (zigbee_device_has(uid)) {
        return;
    }
    interview_device_t *device = interview_slot(uid, short_addr);
    if (device == NULL) {
        ESP_LOGW(TAG, "interview table full: uid=%llx", (unsigned long long)uid);
        return;
    }
    if (device->outstanding > 0) {
        return; /* интервью уже идёт: повторный announce/rejoin его не перезапускает */
    }

    device->endpoint_count = 0;
    device->manufacturer[0] = '\0';
    device->model[0] = '\0';
    device->basic_requested = false;
    device->outstanding = 1;
    ESP_LOGI(TAG, "interview begin: uid=%llx short=0x%04x", (unsigned long long)uid,
             (unsigned)short_addr);

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

void core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (message == NULL) {
        return;
    }

    if (callback_id == EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID) {
        const ezb_zcl_cmd_read_attr_rsp_message_t *rsp =
            (const ezb_zcl_cmd_read_attr_rsp_message_t *)message;
        if (rsp != NULL && rsp->info.cluster_id == EZB_ZCL_CLUSTER_ID_BASIC) {
            interview_on_read_attr(rsp);
        } else {
            state_read_on_attr_rsp(rsp);
        }
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

        /* Первый репорт от устройства после старта хоста. Если устройство уже известно
         * (восстановлено из флеша) — интервью не нужно: оно шлёт репорты по сохранённой
         * на его стороне конфигурации. Интервью — только для неизвестных. */
        if (!configured_seen(zcl_report.device_uid)) {
            configured_add(zcl_report.device_uid);
            if (!zigbee_device_has(zcl_report.device_uid) &&
                report->in.header->src_addr.addr_mode == EZB_ADDR_MODE_SHORT) {
                ESP_LOGI(TAG, "unknown device reported: interview uid=%llx",
                         (unsigned long long)zcl_report.device_uid);
                interview_begin(zcl_report.device_uid, report->in.header->src_addr.u.short_addr);
            }
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
bool raw_frame_handler(const ezb_zcl_raw_frame_t *raw)
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
    if (raw->payload != NULL) {
        const uint16_t len = (raw->payload_length > sizeof(event.payload))
                                 ? (uint16_t)sizeof(event.payload)
                                 : raw->payload_length;
        event.payload_len = (uint8_t)len;
        memcpy(event.payload, raw->payload, len);
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
