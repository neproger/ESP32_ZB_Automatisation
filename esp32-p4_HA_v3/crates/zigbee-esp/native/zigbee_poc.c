/*
 * Минимальный C-слой между стеком Zigbee (esp-zigbee-lib) и Rust.
 *
 * Один координатор на новом разделе zb_storage. Стек живёт в своей задаче и зовёт
 * колбэки оттуда, поэтому наружу выходит только копия события в кольцевой буфер
 * фиксированного размера. Rust читает её опросом, буферы SDK не пересекают границу.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_zigbee.h"
#include "nvs_flash.h"
#include "ezbee/af.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/cluster/on_off.h"
#include "ezbee/zcl/cluster/on_off_desc.h"
#include "ezbee/zcl/cluster/power_config_desc.h"
#include "ezbee/zcl/zcl_core.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "ezbee/zdo/zdo_bind_mgmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POC_UART_PORT UART_NUM_1
#define POC_UART_TX 14 /* P4 GPIO14 -> C6 GPIO20 */
#define POC_UART_RX 15 /* P4 GPIO15 <- C6 GPIO21 */
#define POC_UART_BAUD 460800
#define POC_CHANNEL_MASK (1l << 15)
#define POC_ENDPOINT 1
#define POC_PROFILE_HA 0x0104u
#define POC_DEVICE_GATEWAY 0x0005u
#define POC_OPEN_SECONDS 255
#define POC_QUEUE_LEN 32

static const char *TAG = "zb_poc";

typedef struct {
    uint64_t extended_addr;
    uint16_t short_addr;
    uint16_t cluster_id;
    uint8_t endpoint;
    uint8_t command_id;
    uint8_t payload_len;
    uint8_t payload[8];
} poc_event_t;

static poc_event_t s_queue[POC_QUEUE_LEN];
static volatile uint32_t s_head;
static volatile uint32_t s_tail;
static volatile uint32_t s_dropped;

int zb_poc_poll(poc_event_t *out)
{
    if (s_tail == s_head) {
        return 0;
    }
    *out = s_queue[s_tail % POC_QUEUE_LEN];
    s_tail++;
    return 1;
}

static void queue_push(const poc_event_t *event)
{
    if (s_head - s_tail >= POC_QUEUE_LEN) {
        s_dropped++;
        return;
    }
    s_queue[s_head % POC_QUEUE_LEN] = *event;
    s_head++;
}

static void open_network(void)
{
    const ezb_err_t err = ezb_bdb_open_network(POC_OPEN_SECONDS);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "open network failed: code=0x%x", (unsigned)err);
    }
}

static void bind_result(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    const uint16_t cluster = (uint16_t)(uintptr_t)user_ctx;
    if (result->error == EZB_ERR_NONE && result->rsp != NULL &&
        result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "bound cluster=0x%04x", (unsigned)cluster);
        return;
    }
    ESP_LOGW(TAG, "bind failed cluster=0x%04x error=0x%x status=0x%02x", (unsigned)cluster,
             (unsigned)result->error, result->rsp != NULL ? (unsigned)result->rsp->status : 0xFFu);
}

/* Кнопка шлёт команду только по записи binding на самом устройстве. */
static void bind_device(uint64_t uid, uint16_t short_addr, uint8_t endpoint)
{
    ezb_extaddr_t coordinator = {0};
    ezb_nwk_get_extended_address(&coordinator);

    const uint16_t clusters[] = {0x0006u, 0x0001u};
    for (size_t i = 0; i < sizeof(clusters) / sizeof(clusters[0]); i++) {
        const ezb_zdo_bind_req_t request = {
            .dst_nwk_addr = short_addr,
            .field =
                {
                    .src_addr = {.u64 = uid},
                    .src_ep = endpoint,
                    .cluster_id = clusters[i],
                    .dst_addr_mode = EZB_ADDR_MODE_EXT,
                    .dst_addr = {.extended_addr = coordinator},
                    .dst_ep = POC_ENDPOINT,
                },
            .cb = bind_result,
            .user_ctx = (void *)(uintptr_t)clusters[i],
        };
        if (ezb_zdo_bind_req(&request) != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "bind not sent: cluster=0x%04x ep=%u", (unsigned)clusters[i],
                     (unsigned)endpoint);
        }
    }
}

static bool raw_frame_handler(const ezb_zcl_raw_frame_t *raw)
{
    const ezb_zcl_cmd_hdr_t *header = raw != NULL ? raw->header : NULL;
    if (header == NULL || header->profile_id != POC_PROFILE_HA) {
        return false;
    }
    if (EZB_ZCL_CMD_FC_GET_FRAME_TYPE(header->fc) != EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC ||
        EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(header->fc)) {
        return false;
    }

    poc_event_t event = {0};
    event.short_addr = 0xffffu;
    if (header->src_addr.addr_mode == EZB_ADDR_MODE_EXT) {
        event.extended_addr = header->src_addr.u.extended_addr.u64;
    } else if (header->src_addr.addr_mode == EZB_ADDR_MODE_SHORT) {
        event.short_addr = header->src_addr.u.short_addr;
        ezb_extaddr_t uid = {0};
        if (ezb_address_extended_by_short(event.short_addr, &uid) == EZB_ERR_NONE) {
            event.extended_addr = uid.u64;
        }
    }
    event.cluster_id = header->cluster_id;
    event.endpoint = header->src_ep;
    event.command_id = header->cmd_id;
    if (raw->payload != NULL) {
        const uint16_t len = raw->payload_length > sizeof(event.payload)
                                 ? (uint16_t)sizeof(event.payload)
                                 : raw->payload_length;
        event.payload_len = (uint8_t)len;
        memcpy(event.payload, raw->payload, len);
    }

    ESP_LOGI(TAG, "cmd eui64=%016llx short=0x%04x ep=%u cluster=0x%04x cmd=0x%02x len=%u",
             (unsigned long long)event.extended_addr, (unsigned)event.short_addr,
             (unsigned)event.endpoint, (unsigned)event.cluster_id,
             (unsigned)event.command_id, (unsigned)event.payload_len);
    queue_push(&event);
    return false;
}

static bool app_signal_handler(const ezb_app_signal_t *signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        return true;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_comm_status_t status =
            *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        const bool factory_new = ezb_bdb_is_factory_new();
        ESP_LOGI(TAG, "device startup status=0x%02x factory_new=%d", (unsigned)status,
                 (int)factory_new);
        if (status != EZB_BDB_STATUS_SUCCESS) {
            return true;
        }
        if (factory_new) {
            (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            open_network();
        }
        return true;
    }

    case EZB_BDB_SIGNAL_FORMATION: {
        const ezb_bdb_comm_status_t status =
            *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "network formation status=0x%02x", (unsigned)status);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            open_network();
        }
        return true;
    }

    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        const ezb_zdo_signal_device_authorized_params_t *auth =
            ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "device authorized eui64=%016llx short=0x%04x type=0x%02x status=0x%02x",
                 (unsigned long long)auth->device_addr.u64, (unsigned)auth->short_addr,
                 (unsigned)auth->type, (unsigned)auth->status);
        if (auth->status == 0u) {
            bind_device(auth->device_addr.u64, auth->short_addr, POC_ENDPOINT);
        }
        return true;
    }

    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *annce =
            ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "device announced eui64=%016llx short=0x%04x",
                 (unsigned long long)annce->device_addr.u64, (unsigned)annce->short_addr);
        bind_device(annce->device_addr.u64, annce->short_addr, POC_ENDPOINT);
        return true;
    }

    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const uint8_t duration = *(const uint8_t *)ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "network %s for %u s", duration != 0u ? "open" : "closed",
                 (unsigned)duration);
        return true;
    }

    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *leave =
            ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "device left eui64=%016llx short=0x%04x leave_type=0x%02x",
                 (unsigned long long)leave->device_addr.u64, (unsigned)leave->short_addr,
                 (unsigned)leave->leave_type);
        return true;
    }

    default:
        return true;
    }
}

static void uart_config(esp_zigbee_config_t *config)
{
    config->platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_UART_RCP;

    esp_zigbee_uart_config_t *uart = &config->platform_config.radio_config.radio_uart_config;
    uart->port = POC_UART_PORT;
    uart->uart_config.baud_rate = POC_UART_BAUD;
    uart->uart_config.data_bits = UART_DATA_8_BITS;
    uart->uart_config.parity = UART_PARITY_DISABLE;
    uart->uart_config.stop_bits = UART_STOP_BITS_1;
    uart->uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart->uart_config.rx_flow_ctrl_thresh = 0;
    uart->uart_config.source_clk = UART_SCLK_DEFAULT;
    uart->rx_pin = (gpio_num_t)POC_UART_RX;
    uart->tx_pin = (gpio_num_t)POC_UART_TX;
}

/* --- Attribute Reports -> Rust sink --------------------------------------- */
typedef struct {
    uint64_t src_ext;
    uint16_t src_short;
    uint16_t cluster;
    uint16_t manuf_code;
    uint8_t src_ep;
    uint8_t command;
    uint8_t tsn;
    uint8_t fc;
    int8_t rssi;
} zb_frame_meta_t;

typedef void (*zb_frame_sink_t)(const zb_frame_meta_t *meta, const uint8_t *data, uint16_t len);

static zb_frame_sink_t s_frame_sink;
static uint32_t s_dropped_reports;

void zb_poc_set_frame_sink(zb_frame_sink_t sink) { s_frame_sink = sink; }
uint32_t zb_poc_dropped_reports(void) { return s_dropped_reports; }

/* ZCL type -> value size; 0 means "cannot frame this attribute". */
static size_t zcl_value_size(uint8_t type)
{
    switch (type) {
    case 0x08: return 1;
    case 0x09: return 2;
    case 0x0A: return 3;
    case 0x0B: return 4;
    case 0x0C: return 5;
    case 0x0D: return 6;
    case 0x0E: return 7;
    case 0x0F: return 8;
    case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: return 1;
    case 0x19: case 0x21: case 0x29: case 0x31: return 2;
    case 0x22: case 0x2A: return 3;
    case 0x23: case 0x2B: case 0x39: return 4;
    case 0x27: case 0x2F: case 0x3A: return 8;
    default: return 0;
    }
}

/*
 * SDK отдаёт отчёт списком разобранных атрибутов. Собираем из них канонический
 * Report Attributes payload (attr u16 LE, type, value) в один слот Rust-пула:
 * один колбэк — один кадр. Буфер SDK не покидает колбэк.
 */
static void forward_report(const ezb_zcl_cmd_report_attr_message_t *report)
{
    if (s_frame_sink == NULL || report == NULL || report->in.header == NULL) {
        return;
    }
    uint8_t payload[128];
    uint16_t len = 0;
    for (const ezb_zcl_report_attr_variable_t *var = report->in.variables; var != NULL;
         var = var->next) {
        const size_t size = zcl_value_size(var->attr_type);
        if (size == 0 || var->attr_value == NULL || len + 3u + size > sizeof(payload)) {
            s_dropped_reports++;
            continue;
        }
        payload[len++] = (uint8_t)(var->attr_id & 0xFFu);
        payload[len++] = (uint8_t)(var->attr_id >> 8);
        payload[len++] = var->attr_type;
        memcpy(&payload[len], var->attr_value, size);
        len = (uint16_t)(len + size);
    }
    if (len == 0) {
        return;
    }

    const ezb_zcl_cmd_hdr_t *header = report->in.header;
    zb_frame_meta_t meta = {0};
    meta.cluster = header->cluster_id;
    meta.manuf_code = header->manuf_code;
    meta.src_ep = header->src_ep;
    meta.command = header->cmd_id;
    meta.tsn = header->tsn;
    meta.fc = header->fc;
    meta.rssi = header->rssi;
    if (header->src_addr.addr_mode == EZB_ADDR_MODE_EXT) {
        meta.src_ext = header->src_addr.u.extended_addr.u64;
        (void)ezb_address_short_by_extended(&header->src_addr.u.extended_addr, &meta.src_short);
    } else if (header->src_addr.addr_mode == EZB_ADDR_MODE_SHORT) {
        meta.src_short = header->src_addr.u.short_addr;
        ezb_extaddr_t uid = {0};
        if (ezb_address_extended_by_short(header->src_addr.u.short_addr, &uid) == EZB_ERR_NONE) {
            meta.src_ext = uid.u64;
        }
    }
    s_frame_sink(&meta, payload, len);
}

/* --- Request executor bridge (sends + confirmations) ---------------------- */
typedef void (*zb_exec_confirm_t)(uint32_t op, uint8_t tsn, uint8_t success);
typedef void (*zb_exec_response_t)(uint16_t src_short, uint8_t src_ep, uint16_t cluster,
                                   uint8_t command, uint8_t tsn, int16_t rsp_to_cmd,
                                   int16_t status);
typedef void (*zb_exec_zdo_t)(uint32_t op, uint8_t success);

static zb_exec_confirm_t s_exec_confirm;
static zb_exec_response_t s_exec_response;
static zb_exec_zdo_t s_exec_zdo;

void zb_exec_set_callbacks(zb_exec_confirm_t confirm, zb_exec_response_t response, zb_exec_zdo_t zdo)
{
    s_exec_confirm = confirm;
    s_exec_response = response;
    s_exec_zdo = zdo;
}

static void exec_confirm_cb(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    if (s_exec_confirm != NULL && cnf != NULL) {
        s_exec_confirm((uint32_t)(uintptr_t)user_ctx, cnf->tsn, cnf->status == 0 ? 1 : 0);
    }
}

static void exec_set_addr(ezb_address_t *addr, uint64_t eui64, uint16_t short_addr)
{
    if (eui64 != 0) {
        addr->addr_mode = EZB_ADDR_MODE_EXT;
        addr->u.extended_addr.u64 = eui64;
    } else {
        addr->addr_mode = EZB_ADDR_MODE_SHORT;
        addr->u.short_addr = short_addr;
    }
}

static void exec_notify_response(const ezb_zcl_cmd_hdr_t *header, uint16_t cluster,
                                 uint8_t command, int16_t rsp_to_cmd, int16_t status)
{
    if (s_exec_response == NULL || header == NULL) {
        return;
    }
    uint16_t short_addr = 0;
    if (header->src_addr.addr_mode == EZB_ADDR_MODE_SHORT) {
        short_addr = header->src_addr.u.short_addr;
    } else if (header->src_addr.addr_mode == EZB_ADDR_MODE_EXT) {
        (void)ezb_address_short_by_extended(&header->src_addr.u.extended_addr, &short_addr);
    }
    s_exec_response(short_addr, header->src_ep, cluster, command, header->tsn, rsp_to_cmd,
                    status);
}

int zb_exec_read(uint32_t op, uint64_t eui64, uint16_t short_addr, uint8_t ep, uint16_t cluster,
                 uint16_t attr)
{
    uint16_t attrs[1] = {attr};
    ezb_zcl_read_attr_cmd_t req = {0};
    exec_set_addr(&req.cmd_ctrl.dst_addr, eui64, short_addr);
    req.cmd_ctrl.dst_ep = ep;
    req.cmd_ctrl.src_ep = POC_ENDPOINT;
    req.cmd_ctrl.cluster_id = cluster;
    req.cmd_ctrl.manuf_code = 0; /* standard profile */
    req.cmd_ctrl.cnf_ctx.cb = exec_confirm_cb;
    req.cmd_ctrl.cnf_ctx.user_ctx = (void *)(uintptr_t)op;
    req.payload.attr_number = 1;
    req.payload.attr_field = attrs;

    if (!esp_zigbee_lock_acquire(portMAX_DELAY)) {
        return -1;
    }
    const ezb_err_t err = ezb_zcl_read_attr_cmd_req(&req);
    esp_zigbee_lock_release();
    return err == EZB_ERR_NONE ? 0 : -1;
}

int zb_exec_on_off(uint32_t op, uint64_t eui64, uint16_t short_addr, uint8_t ep, uint8_t on)
{
    ezb_zcl_on_off_cmd_t req = {0};
    exec_set_addr(&req.cmd_ctrl.dst_addr, eui64, short_addr);
    req.cmd_ctrl.dst_ep = ep;
    req.cmd_ctrl.src_ep = POC_ENDPOINT;
    req.cmd_ctrl.dis_default_rsp = false;
    req.cmd_ctrl.cnf_ctx.cb = exec_confirm_cb;
    req.cmd_ctrl.cnf_ctx.user_ctx = (void *)(uintptr_t)op;

    if (!esp_zigbee_lock_acquire(portMAX_DELAY)) {
        return -1;
    }
    const ezb_err_t err =
        on ? ezb_zcl_on_off_on_cmd_req(&req) : ezb_zcl_on_off_off_cmd_req(&req);
    esp_zigbee_lock_release();
    return err == EZB_ERR_NONE ? 0 : -1;
}

int zb_exec_config_report(uint32_t op, uint64_t eui64, uint16_t short_addr, uint8_t ep,
                          uint16_t cluster, uint16_t attr, uint8_t attr_type, uint16_t min,
                          uint16_t max, uint8_t change)
{
    ezb_zcl_config_report_record_t record = {0};
    record.direction = EZB_ZCL_REPORTING_SEND;
    record.attr_id = attr;
    record.client.attr_type = attr_type;
    record.client.min_interval = min;
    record.client.max_interval = max;
    record.client.reportable_change.u8 = change;

    ezb_zcl_config_report_cmd_t req = {0};
    exec_set_addr(&req.cmd_ctrl.dst_addr, eui64, short_addr);
    req.cmd_ctrl.dst_ep = ep;
    req.cmd_ctrl.src_ep = POC_ENDPOINT;
    req.cmd_ctrl.cluster_id = cluster;
    req.cmd_ctrl.manuf_code = 0;
    req.cmd_ctrl.cnf_ctx.cb = exec_confirm_cb;
    req.cmd_ctrl.cnf_ctx.user_ctx = (void *)(uintptr_t)op;
    req.payload.record_number = 1;
    req.payload.record_field = &record;

    if (!esp_zigbee_lock_acquire(portMAX_DELAY)) {
        return -1;
    }
    const ezb_err_t err = ezb_zcl_config_report_cmd_req(&req);
    esp_zigbee_lock_release();
    return err == EZB_ERR_NONE ? 0 : -1;
}

static void exec_bind_cb(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    const bool ok = result != NULL && result->error == EZB_ERR_NONE &&
                    result->rsp != NULL && result->rsp->status == EZB_ZDP_STATUS_SUCCESS;
    if (s_exec_zdo != NULL) {
        s_exec_zdo((uint32_t)(uintptr_t)user_ctx, ok ? 1 : 0);
    }
}

int zb_exec_bind(uint32_t op, uint64_t eui64, uint16_t short_addr, uint8_t src_ep,
                 uint16_t cluster, uint8_t dst_ep)
{
    ezb_extaddr_t coordinator = {0};
    ezb_nwk_get_extended_address(&coordinator);
    if (short_addr == 0 && eui64 != 0) {
        ezb_extaddr_t device = {.u64 = eui64};
        (void)ezb_address_short_by_extended(&device, &short_addr);
    }
    if (short_addr == 0) {
        return -1;
    }

    ezb_zdo_bind_req_t req = {0};
    req.dst_nwk_addr = short_addr;
    req.field.src_addr.u64 = eui64;
    req.field.src_ep = src_ep;
    req.field.cluster_id = cluster;
    req.field.dst_addr_mode = EZB_ADDR_MODE_EXT;
    req.field.dst_addr.extended_addr = coordinator;
    req.field.dst_ep = dst_ep;
    req.cb = exec_bind_cb;
    req.user_ctx = (void *)(uintptr_t)op;

    if (!esp_zigbee_lock_acquire(portMAX_DELAY)) {
        return -1;
    }
    const ezb_err_t err = ezb_zdo_bind_req(&req);
    esp_zigbee_lock_release();
    return err == EZB_ERR_NONE ? 0 : -1;
}

/* --- end request executor bridge ----------------------------------------- */

static void poc_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (message == NULL) {
        return;
    }
    switch (callback_id) {
    case EZB_ZCL_CORE_REPORT_ATTR_CB_ID: {
        ezb_zcl_cmd_report_attr_message_t *report = message;
        report->out.result = EZB_ZCL_STATUS_SUCCESS;
        forward_report(report);
        break;
    }
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID: {
        ezb_zcl_cmd_read_attr_rsp_message_t *rsp = message;
        rsp->out.result = EZB_ZCL_STATUS_SUCCESS;
        exec_notify_response(rsp->in.header, rsp->info.cluster_id, 0x01, -1, -1);
        break;
    }
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *rsp = message;
        exec_notify_response(rsp->in.header, rsp->info.cluster_id, 0x0B,
                             (int16_t)rsp->in.rsp_to_cmd, (int16_t)rsp->in.status_code);
        break;
    }
    case EZB_ZCL_CORE_CONFIG_REPORT_RSP_CB_ID: {
        ezb_zcl_cmd_config_report_rsp_message_t *rsp = message;
        rsp->out.result = EZB_ZCL_STATUS_SUCCESS;
        int16_t status = 0;
        for (const ezb_zcl_config_report_rsp_variable_t *var = rsp->in.variables; var != NULL;
             var = var->next) {
            if (var->status != EZB_ZCL_STATUS_SUCCESS) {
                status = 1;
                break;
            }
        }
        exec_notify_response(rsp->in.header, rsp->info.cluster_id, 0x07, -1, status);
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
static bool coordinator_device(void)
{
    ezb_af_device_desc_t device = ezb_af_create_device_desc();

    ezb_af_ep_config_t config = {0};
    config.ep_id = POC_ENDPOINT;
    config.app_profile_id = POC_PROFILE_HA;
    config.app_device_id = POC_DEVICE_GATEWAY;
    ezb_af_ep_desc_t endpoint = ezb_af_create_gateway_endpoint(&config);
    if (endpoint == NULL) {
        ESP_LOGE(TAG, "endpoint desc not created");
        return false;
    }

    ezb_zcl_cluster_desc_t on_off =
        ezb_zcl_on_off_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    if (on_off != NULL) {
        (void)ezb_af_endpoint_add_cluster_desc(endpoint, on_off);
    }
    ezb_zcl_cluster_desc_t power =
        ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    if (power != NULL) {
        (void)ezb_af_endpoint_add_cluster_desc(endpoint, power);
    }

    if (ezb_af_device_add_endpoint_desc(device, endpoint) != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "endpoint not added to device");
        return false;
    }
    if (ezb_af_device_desc_register(device) != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "device desc not registered");
        return false;
    }
    ezb_zcl_raw_command_handler_register(raw_frame_handler);
    ezb_zcl_core_action_handler_register(poc_core_action_handler);
    return true;
}

esp_err_t zb_poc_start(void)
{
    /* shortcut: PoC держит сеть в общем разделе nvs; при переносе в V3 вернуть свой zb_storage. */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs not usable (%s), erasing", esp_err_to_name(nvs));
        nvs = nvs_flash_erase();
        if (nvs == ESP_OK) {
            nvs = nvs_flash_init();
        }
    }
    if (nvs != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(nvs));
        return nvs;
    }

    esp_zigbee_config_t config = {
        .device_config =
            {
                .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,
                .install_code_policy = false,
                .zczr_config = { .max_children = 10 },
            },
        .platform_config = { .storage_partition_name = "nvs" },
    };
    uart_config(&config);

    /* RCP на C6 стартует одновременно с P4: дать ему подняться до хендшейка. */
    vTaskDelay(pdMS_TO_TICKS(1000));

    const esp_err_t inited = esp_zigbee_init(&config);
    if (inited != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_init failed: %s", esp_err_to_name(inited));
        return inited;
    }

    ezb_aps_secur_enable_distributed_security(false);
    (void)ezb_secur_set_tclk_exchange_required(false);
    ezb_secur_tcpol_set_allow_rejoins_with_well_known_key(true);

    const ezb_err_t channel = ezb_bdb_set_primary_channel_set(POC_CHANNEL_MASK);
    if (channel != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "channel set failed: code=0x%x", (unsigned)channel);
        return ESP_FAIL;
    }
    if (ezb_app_signal_add_handler(app_signal_handler) != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "signal handler not added");
        return ESP_FAIL;
    }
    if (!coordinator_device()) {
        return ESP_FAIL;
    }

    const esp_err_t started = esp_zigbee_start(false);
    if (started != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_start failed: %s", esp_err_to_name(started));
        return started;
    }
    ESP_LOGI(TAG, "stack started, waiting for network");
    return ESP_OK;
}

/* Стек обрабатывает сигналы в задаче приложения: без этого цикла BDB не оживёт. */
void zb_poc_run(void)
{
    const esp_err_t err = esp_zigbee_launch_mainloop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mainloop exited: %s", esp_err_to_name(err));
    }
}
