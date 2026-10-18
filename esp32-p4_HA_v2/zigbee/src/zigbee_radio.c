#include "zigbee/zigbee_radio.h"

#include "zigbee_radio_internal.h"

#include <stdbool.h>
#include <string.h>

#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_zigbee.h"
#include "ezbee/af.h"
#include "ezbee/app_signals.h"
#include "ezbee/aps.h"
#include "ezbee/bdb.h"
#include "ezbee/zcl/zcl_core.h"
#include "zigbee/zigbee_adapter_ids.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_binding.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * Радио: подъём стека Zigbee на P4 и RCP (802.15.4) на C6 по UART (прошивка ot_rcp,
 * standalone spinel). Задача радио поднимает стек и уходит в mainloop; отправка команд
 * и открытие сети — отсюда. Обработчики report/интервью — zigbee_radio_reports.c,
 * commissioning/leave — zigbee_radio_commissioning.c.
 */

static const char *TAG = "zigbee.radio";

#define RADIO_TASK_STACK 8192
#define RADIO_TASK_PRIORITY 5
#define RADIO_START_TIMEOUT_MS 15000
#define PRIMARY_CHANNEL_MASK (1l << 15)
#define ZIGBEE_STORAGE_PARTITION "zb_storage"

/* Spinel-UART к ot_rcp на C6 по разведённым линиям SDIO: P4 GPIO14(TX)/15(RX)
 * <-> C6 GPIO20(RX)/21(TX). Внешние перемычки не нужны (docs/hardware §4.2.1). */
#define RCP_UART_PORT UART_NUM_1
#define RCP_UART_TX_PIN 14
#define RCP_UART_RX_PIN 15
#define RCP_UART_BAUD 460800
#define RCP_START_DELAY_MS 1000

/* Стек готов принимать команды. Пишет задача радио при старте, читает задача сервиса. */
volatile bool zigbee_radio_ready;

/*
 * Исход старта асинхронной задачи радио; bootstrap ждёт его на семафоре. Память
 * семафора статическая: задача отдаёт его ровно раз, удалять не нужно.
 */
static StaticSemaphore_t s_radio_init_done_storage;
static SemaphoreHandle_t s_radio_init_done;
static sys_error_t s_radio_init_error;

/* Сбой подъёма радио — ошибка слоя Zigbee; задача сообщает её bootstrap'у.
 * code — сырой код SDK для диагностики; 0, когда исходного кода нет. */
static sys_error_t radio_fail(const char *what, uint32_t code)
{
    if (code != 0u) {
        ESP_LOGE(TAG, "%s: code=0x%x", what, (unsigned)code);
    } else {
        ESP_LOGE(TAG, "%s", what);
    }
    return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_IO);
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
    return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_IO);
}

/*
 * Отправка любой ZCL cluster-specific команды: собираем ZCL-кадр (frame control, seq,
 * cmd id, args) и шлём через APS. Список команд и кодирование аргументов задаёт словарь
 * (ha_zigbee.h на бэке, web-ui на фронте) — специальных путей под конкретный кластер нет.
 */
static uint8_t s_zcl_seq;

static sys_error_t send_raw_zcl(const ha_zb_command_t *command)
{
    uint8_t frame[3 + HA_ZB_COMMAND_ARGS_MAX];
    frame[0] = 0x01; /* cluster-specific, client→server, default response enabled */
    frame[1] = s_zcl_seq++;
    frame[2] = command->command_id;
    if (command->args_len > 0) {
        memcpy(&frame[3], command->args, command->args_len);
    }

    const ezb_extaddr_t eui = {.u64 = command->device_uid};
    const ezb_apsde_data_req_t request = {
        .dst_address = EZB_ADDRESS_EXTENDED(eui),
        .src_endpoint = COORDINATOR_ENDPOINT,
        .dst_endpoint = command->dst_endpoint,
        .cluster_id = command->cluster_id,
        .profile_id = HA_ZB_PROFILE_HA,
        .radius = 0x1E,
        .tx_options = EZB_APSDE_TX_OPT_SECURITY_ENABLED,
        .asdu_length = (uint16_t)(3 + command->args_len),
        .asdu = frame,
    };
    return radio_error(ezb_apsde_data_request(&request));
}

sys_error_t zigbee_radio_send(const ha_zb_command_t *command)
{
    if (command == NULL) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_INVALID_ARG);
    }
    if (!zigbee_radio_ready) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_INVALID_STATE);
    }

    return send_raw_zcl(command);
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
        zigbee_radio_ready = true;
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
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_INVALID_ARG);
    }

    s_radio_init_done = xSemaphoreCreateBinaryStatic(&s_radio_init_done_storage);
    if (s_radio_init_done == NULL) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_NO_MEM);
    }

    if (xTaskCreate(zigbee_stack_task, "zigbee_radio", RADIO_TASK_STACK, NULL, RADIO_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_NO_MEM);
    }

    /*
     * Без Zigbee система не поднимается: исход старта должен дойти до bootstrap'а, а не
     * потеряться в отсоединённой задаче. Таймаут страхует от зависшего подъёма: задачу
     * мы не отменяем, поэтому она может отработать позже, но решение уже принято
     * (services/ZIGBEE.md §5).
     */
    if (xSemaphoreTake(s_radio_init_done, pdMS_TO_TICKS(RADIO_START_TIMEOUT_MS)) != pdTRUE) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_IO);
    }
    return s_radio_init_error;
}
