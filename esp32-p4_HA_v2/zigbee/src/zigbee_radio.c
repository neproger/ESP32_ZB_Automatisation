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
#include "zigbee/zigbee.h"
#include "ezbee/zcl/cluster/level.h"
#include "ezbee/zcl/cluster/on_off.h"
#include "ezbee/zcl/zcl_core.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "esp_hosted.h"
#include "esp_hosted_openthread.h"
#include "freertos/FreeRTOS.h"
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
#define COORDINATOR_ENDPOINT 1
#define PRIMARY_CHANNEL_MASK (1l << 15)
#define ZIGBEE_STORAGE_PARTITION "zb_storage"

/* Стек готов принимать команды. Пишет задача радио при старте, читает задача сервиса. */
static volatile bool s_ready;

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

static void core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (callback_id != EZB_ZCL_CORE_REPORT_ATTR_CB_ID || message == NULL) {
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

static bool app_signal_handler(const ezb_app_signal_t *signal)
{
    ESP_LOGI(TAG, "signal 0x%04x", (unsigned)ezb_app_signal_get_type(signal));
    return false;
}

static void create_coordinator_device(void)
{
    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    ezb_af_ep_config_t ep_config = {0};
    ep_config.ep_id = COORDINATOR_ENDPOINT;
    ep_config.app_profile_id = HA_ZB_PROFILE_HA;
    ep_config.app_device_id = 0x0005u;

    ezb_af_ep_desc_t endpoint = ezb_af_create_gateway_endpoint(&ep_config);
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device, endpoint));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(device));
    ezb_zcl_core_action_handler_register(core_action_handler);
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

static void zigbee_stack_task(void *arg)
{
    (void)arg;

    nvs_flash_init();
    nvs_flash_init_partition(ZIGBEE_STORAGE_PARTITION);

    if (esp_hosted_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init failed");
        vTaskDelete(NULL);
        return;
    }
    if (esp_hosted_connect_to_slave() != ESP_OK) {
        ESP_LOGE(TAG, "slave over SDIO not connected");
        vTaskDelete(NULL);
        return;
    }
    /* Пример esp_hosted делает то же в esp_hosted_openthread_app_init(); заголовок того
     * вспомогательного файла в компонент не входит, поэтому шаги расписаны здесь. */
    if (esp_hosted_openthread_rcp_query(HOSTED_OPENTHREAD_QUERY_CONFIGURED) != ESP_OK ||
        esp_hosted_openthread_rcp_init() != ESP_OK ||
        esp_hosted_openthread_rcp_query(HOSTED_OPENTHREAD_QUERY_INITED) != ESP_OK ||
        esp_hosted_openthread_rcp_start() != ESP_OK ||
        esp_hosted_openthread_rcp_query(HOSTED_OPENTHREAD_QUERY_ENABLED) != ESP_OK ||
        esp_hosted_openthread_rcp_query(HOSTED_OPENTHREAD_QUERY_READY) != ESP_OK) {
        ESP_LOGE(TAG, "RCP on the co-processor not started");
        vTaskDelete(NULL);
        return;
    }

    esp_hosted_openthread_radio_config_t radio = {0};
    if (esp_hosted_openthread_get_radio_config(&radio) != 0 ||
        radio.type != HOSTED_OPENTHREAD_TRANSPORT_UART) {
        ESP_LOGE(TAG, "expected UART spinel transport to RCP");
        vTaskDelete(NULL);
        return;
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

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));

    create_coordinator_device();

    ESP_ERROR_CHECK(esp_zigbee_start(false));

    s_ready = true;
    esp_zigbee_launch_mainloop();

    vTaskDelete(NULL);
}

sys_error_t zigbee_radio_start(domain_t *domain)
{
    if (domain == NULL) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_ARG);
    }

    if (xTaskCreate(zigbee_stack_task, "zigbee_radio", RADIO_TASK_STACK, NULL, RADIO_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
