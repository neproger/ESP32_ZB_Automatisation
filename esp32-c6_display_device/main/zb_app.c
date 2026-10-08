#include "zb_app.h"
#include "board.h"
#include "button.h"
#include "rgb_led.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/zcl/zcl_core.h"
#include "ezbee/zcl/cluster/color_control_desc.h"
#include "ezbee/zcl/zcl_reporting.h"

static const char *TAG = "zb_app";

/* Endpoints */
#define HA_SWITCH_EP        1   /* On/Off switch (client) - the button */
#define HA_LIGHT_EP         2   /* Color dimmable light (server) - the RGB LED */
#define HA_TEMP_EP          3   /* Temperature sensor (server) - on-chip die temp */

/* Temperature reporting interval */
#define TEMP_REPORT_PERIOD_US  (10 * 1000 * 1000)

/* Coordinator target for button commands (coordinator short addr / endpoint) */
#define COORDINATOR_SHORT   0x0000
#define COORDINATOR_EP      1

/* Channels 11..26 */
#define PRIMARY_CHANNEL_MASK  0x07FFF800U

/* Manufacturer / model (length-prefixed Zigbee strings) */
#define ESP_MANUFACTURER_NAME "\x09""ESPRESSIF"
#define ESP_MODEL_IDENTIFIER  "\x0F""ESP32C6-DISPLAY"

#define ESP_ZIGBEE_ZR_CONFIG()                                                    \
    {                                                                            \
        .device_config = {                                                        \
            .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,                            \
            .install_code_policy = false,                                         \
            .zczr_config = {                                                      \
                .max_children = 10,                                               \
            },                                                                    \
        },                                                                        \
        .platform_config = {                                                      \
            .storage_partition_name = "nvs",                                      \
            .radio_config = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE },        \
        },                                                                        \
    }

/* Lamp state (ZCL units) */
static bool s_light_on;
static uint8_t s_level = 254;
static uint8_t s_hue = 0;
static uint8_t s_sat = 254;
static uint16_t s_color_x = 0x6161;
static uint16_t s_color_y = 0x6161;
static uint16_t s_color_temp = EZB_ZCL_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_DEFAULT_VALUE;
static uint8_t s_color_mode = EZB_ZCL_COLOR_CONTROL_COLOR_MODE_DEFAULT_VALUE;

static uint32_t s_button_count;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_temp_timer;
static temperature_sensor_handle_t s_temp_sensor;

static void format_ieee(char *out, size_t out_len, const ezb_extaddr_t *addr)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             addr->u8[7], addr->u8[6], addr->u8[5], addr->u8[4],
             addr->u8[3], addr->u8[2], addr->u8[1], addr->u8[0]);
}

static void update_net_info(void)
{
    ezb_extaddr_t ieee = {0};
    ezb_nwk_get_extended_address(&ieee);
    char ieee_str[24];
    format_ieee(ieee_str, sizeof(ieee_str), &ieee);
    ui_set_net_info(ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address(), ieee_str);
}

static void apply_light(void)
{
    if (!s_light_on || s_level == 0) {
        rgb_led_off();
    } else if (s_color_mode == EZB_ZCL_COLOR_CONTROL_COLOR_MODE_CURRENT_HUE_AND_CURRENT_SATURATION) {
        rgb_led_set_hs(s_hue, s_sat, s_level);
    } else if (s_color_mode == EZB_ZCL_COLOR_CONTROL_COLOR_MODE_COLOR_TEMPERATURE_MIREDS) {
        rgb_led_set_ct(s_color_temp, s_level);
    } else {
        rgb_led_set_xy(s_color_x, s_color_y, s_level);
    }
    ui_set_light(s_light_on, s_level, s_hue, s_sat);
}

static void read_and_report_temperature(void)
{
    if (!s_temp_sensor) {
        return;
    }
    float celsius = 0.0f;
    if (temperature_sensor_get_celsius(s_temp_sensor, &celsius) != ESP_OK) {
        return;
    }

    /* ZCL MeasuredValue for temperature is int16 in 0.01 degC units. */
    int16_t measured = (int16_t)(celsius * 100.0f);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_status_t status = ezb_zcl_set_attr_value(HA_TEMP_EP, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
                                                     EZB_ZCL_CLUSTER_SERVER,
                                                     EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID,
                                                     EZB_ZCL_STD_MANUF_CODE, &measured, false);
    esp_zigbee_lock_release();

    if (status == EZB_ZCL_STATUS_SUCCESS) {
        ui_set_temperature(celsius);
    }
    ESP_LOGI(TAG, "die temp %.2f C (0x%04x), status 0x%02x", celsius, (unsigned)(uint16_t)measured, status);
}

static void temp_timer_cb(void *arg)
{
    (void)arg;
    read_and_report_temperature();
}

static void start_temperature_reporting(void)
{
    static bool started;
    if (started) {
        return;
    }
    ezb_zcl_reporting_info_t info =
        ezb_zcl_reporting_info_find(HA_TEMP_EP, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT, EZB_ZCL_CLUSTER_SERVER,
                                    EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID, EZB_ZCL_STD_MANUF_CODE);
    if (info == EZB_ZCL_INVALID_REPORTING_INFO) {
        ESP_LOGW(TAG, "temperature reporting info not found");
        return;
    }
    ezb_zcl_attr_variable_t delta = { .s16 = 50 }; /* report on 0.50 degC change */
    if (ezb_zcl_reporting_info_update(info, 5, 60, &delta) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "temperature reporting update failed");
        return;
    }
    (void)ezb_zcl_reporting_start_attr_report(info);
    started = true;
    ESP_LOGI(TAG, "temperature reporting started (5..60 s)");
}

/*
 * Цвет: штатный ZHA-конфиг цветного светильника не делает Color-атрибуты reportable —
 * координатор получает 0x86 на Configure Reporting. Поэтому включаем отчёты локально;
 * они уходят на сбинженный координатор (bind на 0x0300 делает хост).
 */
static void start_color_reporting(void)
{
    static const struct {
        uint16_t attr;
        bool is_u16;
    } attrs[] = {
        {EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID, true},
        {EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID, true},
        {EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID, false},
        {EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID, false},
        {EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID, true},
    };

    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
        ezb_zcl_reporting_info_t info =
            ezb_zcl_reporting_info_find(HA_LIGHT_EP, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                                        EZB_ZCL_CLUSTER_SERVER, attrs[i].attr, EZB_ZCL_STD_MANUF_CODE);
        if (info == EZB_ZCL_INVALID_REPORTING_INFO) {
            ESP_LOGW(TAG, "color attr 0x%04x: no reporting info", attrs[i].attr);
            continue;
        }
        ezb_zcl_attr_variable_t delta = {0};
        if (attrs[i].is_u16) {
            delta.u16 = 1;
        } else {
            delta.u8 = 1;
        }
        (void)ezb_zcl_reporting_info_update(info, 0, 3600, &delta);
        (void)ezb_zcl_reporting_start_attr_report(info);
        ESP_LOGI(TAG, "color attr 0x%04x: reporting started", attrs[i].attr);
    }
}

/* --------------------------- Zigbee callbacks ---------------------------- */

static void core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (callback_id != EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID) {
        return;
    }

    ezb_zcl_set_attr_value_message_t *msg = (ezb_zcl_set_attr_value_message_t *)message;
    if (!msg || msg->info.cluster_role != EZB_ZCL_CLUSTER_SERVER || msg->info.dst_ep != HA_LIGHT_EP) {
        return;
    }

    ezb_zcl_attribute_t *attr = &msg->in.attribute;
    if (!attr->data.value) {
        return;
    }

    switch (msg->info.cluster_id) {
    case EZB_ZCL_CLUSTER_ID_ON_OFF:
        if (attr->id == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            s_light_on = (*(uint8_t *)attr->data.value) != 0;
            ui_add_log("RX OnOff: %s", s_light_on ? "ON" : "OFF");
            apply_light();
        }
        break;
    case EZB_ZCL_CLUSTER_ID_LEVEL:
        if (attr->id == EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID) {
            s_level = *(uint8_t *)attr->data.value;
            ui_add_log("RX Level: %u", s_level);
            apply_light();
        }
        break;
    case EZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
        switch (attr->id) {
        case EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID:
            s_color_x = *(uint16_t *)attr->data.value;
            s_color_mode = EZB_ZCL_COLOR_CONTROL_COLOR_MODE_CURRENT_X_AND_CURRENT_Y;
            ui_add_log("RX Color x=0x%04X", s_color_x);
            apply_light();
            break;
        case EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID:
            s_color_y = *(uint16_t *)attr->data.value;
            s_color_mode = EZB_ZCL_COLOR_CONTROL_COLOR_MODE_CURRENT_X_AND_CURRENT_Y;
            ui_add_log("RX Color y=0x%04X", s_color_y);
            apply_light();
            break;
        case EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID:
            s_hue = *(uint8_t *)attr->data.value;
            s_color_mode = EZB_ZCL_COLOR_CONTROL_COLOR_MODE_CURRENT_HUE_AND_CURRENT_SATURATION;
            ui_add_log("RX Hue: %u", s_hue);
            apply_light();
            break;
        case EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID:
            s_sat = *(uint8_t *)attr->data.value;
            ui_add_log("RX Sat: %u", s_sat);
            apply_light();
            break;
        case EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID:
            s_color_temp = *(uint16_t *)attr->data.value;
            s_color_mode = EZB_ZCL_COLOR_CONTROL_COLOR_MODE_COLOR_TEMPERATURE_MIREDS;
            ui_add_log("RX ColorTemp: %u mireds", s_color_temp);
            apply_light();
            break;
        case EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID:
            s_color_mode = *(uint8_t *)attr->data.value;
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

/*
 * Явная обработка команд Color Control. Стек не всегда применяет cluster-specific
 * команды цвета к атрибутам (SET_ATTR_VALUE не срабатывает), поэтому берём их сами.
 * Возвращаем false — пусть стек тоже обработает и ответит Default Response.
 */
#define HA_PROFILE_ID 0x0104U

static void set_color_attr(uint16_t attr_id, const void *value)
{
    (void)ezb_zcl_set_attr_value(HA_LIGHT_EP, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                                 EZB_ZCL_CLUSTER_SERVER, attr_id, EZB_ZCL_STD_MANUF_CODE,
                                 (void *)value, false);
}

static bool raw_command_handler(const ezb_zcl_raw_frame_t *raw)
{
    const ezb_zcl_cmd_hdr_t *h = raw != NULL ? raw->header : NULL;
    if (h == NULL || h->profile_id != HA_PROFILE_ID ||
        EZB_ZCL_CMD_FC_GET_FRAME_TYPE(h->fc) != EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC ||
        EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(h->fc)) {
        return false;
    }
    if (h->cluster_id != EZB_ZCL_CLUSTER_ID_COLOR_CONTROL || h->dst_ep != HA_LIGHT_EP) {
        return false;
    }

    const uint8_t *p = raw->payload;
    if (h->cmd_id == EZB_ZCL_CMD_COLOR_CONTROL_MOVE_TO_COLOR_TEMPERATURE_ID &&
        raw->payload_length >= 2) {
        s_color_temp = (uint16_t)(p[0] | (p[1] << 8));
        s_color_mode = EZB_ZCL_COLOR_CONTROL_COLOR_MODE_COLOR_TEMPERATURE_MIREDS;
        /* Пишем и ZCL-атрибуты: reporting (X/Y/CT) уходит только на их изменение. */
        set_color_attr(EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_MIREDS_ID, &s_color_temp);
        set_color_attr(EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &s_color_mode);
        ui_add_log("RX ColorTemp: %u mireds", s_color_temp);
        apply_light();
    } else if (h->cmd_id == EZB_ZCL_CMD_COLOR_CONTROL_MOVE_TO_COLOR_ID &&
               raw->payload_length >= 4) {
        s_color_x = (uint16_t)(p[0] | (p[1] << 8));
        s_color_y = (uint16_t)(p[2] | (p[3] << 8));
        s_color_mode = EZB_ZCL_COLOR_CONTROL_COLOR_MODE_CURRENT_X_AND_CURRENT_Y;
        set_color_attr(EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID, &s_color_x);
        set_color_attr(EZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID, &s_color_y);
        set_color_attr(EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID, &s_color_mode);
        ui_add_log("RX Color x=0x%04X y=0x%04X", s_color_x, s_color_y);
        apply_light();
    }
    return false;
}

static void retry_commissioning_cb(void *arg)
{
    (void)arg;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
}

static void schedule_steering_retry(void)
{
    if (!s_retry_timer) {
        const esp_timer_create_args_t args = {
            .callback = retry_commissioning_cb,
            .name = "zb_retry",
        };
        if (esp_timer_create(&args, &s_retry_timer) != ESP_OK) {
            return;
        }
    }
    (void)esp_timer_stop(s_retry_timer);
    (void)esp_timer_start_once(s_retry_timer, 3 * 1000 * 1000);
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    const ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack startup");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal);
        if (status != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "%s failed (0x%02x), retry", ezb_app_signal_to_string(signal_type), status);
            schedule_steering_retry();
            break;
        }
        if (ezb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "Factory new: start network steering");
            ui_set_network_state("JOINING", 0xffb300);
            ui_add_log("Network steering...");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGI(TAG, "Reboot on existing network");
            ui_set_network_state("NETWORK OK", 0x66bb6a);
            update_net_info();
            start_temperature_reporting();
            start_color_reporting();
        }
        break;
    }

    case EZB_BDB_SIGNAL_STEERING: {
        const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Joined network: PAN 0x%04hx, ch %u, short 0x%04hx",
                     ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            ui_set_network_state("JOINED", 0x66bb6a);
            update_net_info();
            start_temperature_reporting();
            start_color_reporting();
            ui_add_log("Joined 0x%04hx ch%u", ezb_nwk_get_panid(), ezb_nwk_get_current_channel());
        } else {
            ESP_LOGW(TAG, "Steering failed (0x%02x), retry", status);
            ui_set_network_state("JOINING", 0xffb300);
            schedule_steering_retry();
        }
        break;
    }

    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const uint8_t duration = *(const uint8_t *)ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Permit join: %u s", duration);
        break;
    }

    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *p =
            (const ezb_zdo_signal_leave_indication_params_t *)ezb_app_signal_get_params(app_signal);
        if (p) {
            ESP_LOGW(TAG, "Node 0x%04hx left", p->short_addr);
        }
        break;
    }

    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *p =
            (const ezb_zdo_signal_leave_params_t *)ezb_app_signal_get_params(app_signal);
        ESP_LOGW(TAG, "Left network (type=%u)", p ? p->leave_type : 0);
        ui_set_network_state("OFFLINE", 0xef5350);
        ui_add_log("Left network");
        break;
    }

    default:
        break;
    }
    return true;
}

/* ----------------------------- Button action ----------------------------- */

static void send_on_off_toggle(void)
{
    ezb_zcl_on_off_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr = EZB_ADDRESS_SHORT(COORDINATOR_SHORT),
            .dst_ep = COORDINATOR_EP,
            .src_ep = HA_SWITCH_EP,
        },
    };
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = ezb_zcl_on_off_toggle_cmd_req(&cmd);
    esp_zigbee_lock_release();

    if (err == EZB_ERR_NONE) {
        ui_add_log("TX OnOff Toggle");
    } else {
        ui_add_log("TX failed 0x%04x", err);
    }
    ESP_LOGI(TAG, "On/Off toggle -> coordinator, err=0x%04x", err);
}

static void button_event_handler(button_event_t event, void *user_ctx)
{
    (void)user_ctx;
    if (event == BUTTON_EVENT_LONG_PRESS) {
        ESP_LOGW(TAG, "Long press: factory reset");
        ui_set_network_state("RESET", 0xef5350);
        ui_add_log("Factory reset");
        esp_zigbee_lock_acquire(portMAX_DELAY);
        esp_zigbee_factory_reset();
        return;
    }

    s_button_count++;
    ui_set_button_count(s_button_count);
    send_on_off_toggle();
}

/* ----------------------------- Device setup ------------------------------ */

static void add_basic_info(ezb_af_ep_desc_t ep)
{
    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    if (basic) {
        (void)ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, ESP_MANUFACTURER_NAME);
        (void)ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, ESP_MODEL_IDENTIFIER);
    }
}

static void add_light_color_info(ezb_af_ep_desc_t ep)
{
    ezb_zcl_cluster_desc_t color =
        ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_CLUSTER_SERVER);
    if (!color) {
        return;
    }
    uint16_t min_mireds = 100; /* 10000 K */
    uint16_t max_mireds = 500; /* 2000 K */
    (void)ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID,
                                                      &min_mireds);
    (void)ezb_zcl_color_control_cluster_desc_add_attr(color, EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID,
                                                      &max_mireds);
}

static void log_simple_descriptors(void)
{
    ESP_LOGI(TAG, "AF max endpoint num = %u", ezb_af_dev_get_max_endpoint_num());
    const ezb_af_simple_desc_t *sd = NULL;
    while ((sd = ezb_af_get_next_simple_desc(sd)) != NULL) {
        ESP_LOGI(TAG, "SimpleDesc ep=%u profile=0x%04X dev=0x%04X in=%u out=%u",
                 sd->ep_id, sd->app_profile_id, sd->app_device_id,
                 sd->app_input_cluster_count, sd->app_output_cluster_count);
    }

    uint16_t caps = 0;
    ezb_zcl_attr_desc_t cap = ezb_zcl_get_attr_desc(HA_LIGHT_EP, EZB_ZCL_CLUSTER_ID_COLOR_CONTROL, EZB_ZCL_CLUSTER_SERVER,
                                                    EZB_ZCL_ATTR_COLOR_CONTROL_COLOR_CAPABILITIES_ID, EZB_ZCL_STD_MANUF_CODE);
    if (cap && ezb_zcl_attr_desc_get_value(cap, &caps) == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "ColorCapabilities=0x%04X", caps);
    }
}

static esp_err_t create_device(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ESP_RETURN_ON_FALSE(dev_desc != EZB_INVALID_AF_DEVICE_DESC, ESP_FAIL, TAG, "device desc");

    /* Allow several endpoints (IDs up to 16). */
    (void)ezb_af_dev_set_max_endpoint_num(16);

    /* Endpoint 1: standard HA On/Off switch (client) */
    ezb_zha_on_off_switch_config_t switch_cfg = EZB_ZHA_ON_OFF_SWITCH_CONFIG();
    ezb_af_ep_desc_t switch_ep = ezb_zha_create_on_off_switch(HA_SWITCH_EP, &switch_cfg);
    ESP_RETURN_ON_FALSE(switch_ep != EZB_INVALID_AF_EP_DESC, ESP_FAIL, TAG, "switch ep");
    add_basic_info(switch_ep);
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, switch_ep), TAG, "add switch ep");

    /* Endpoint 2: standard HA Color Dimmable Light (server) */
    ezb_zha_color_dimmable_light_config_t light_cfg = EZB_ZHA_COLOR_DIMMABLE_LIGHT_CONFIG();
    light_cfg.color_cfg.color_capabilities = EZB_ZCL_COLOR_CONTROL_COLOR_CAPABILITIES_HUE_SATURATION_SUPPORTED |
                                             EZB_ZCL_COLOR_CONTROL_COLOR_CAPABILITIES_XY_SUPPORTED |
                                             EZB_ZCL_COLOR_CONTROL_COLOR_CAPABILITIES_COLOR_TEMPERATURE_SUPPORTED;
    ezb_af_ep_desc_t light_ep = ezb_zha_create_color_dimmable_light(HA_LIGHT_EP, &light_cfg);
    ESP_RETURN_ON_FALSE(light_ep != EZB_INVALID_AF_EP_DESC, ESP_FAIL, TAG, "light ep");
    add_basic_info(light_ep);
    add_light_color_info(light_ep);
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, light_ep), TAG, "add light ep");

    /* Endpoint 3: standard HA Temperature Sensor (server, on-chip die temperature) */
    ezb_zha_temperature_sensor_config_t temp_cfg = EZB_ZHA_TEMPERATURE_SENSOR_CONFIG();
    temp_cfg.temp_meas_cfg.measured_value = EZB_ZCL_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_DEFAULT_VALUE;
    temp_cfg.temp_meas_cfg.min_measured_value = -1000;  /* -10.00 degC */
    temp_cfg.temp_meas_cfg.max_measured_value = 12500;  /* 125.00 degC */
    ezb_af_ep_desc_t temp_ep = ezb_zha_create_temperature_sensor(HA_TEMP_EP, &temp_cfg);
    ESP_RETURN_ON_FALSE(temp_ep != EZB_INVALID_AF_EP_DESC, ESP_FAIL, TAG, "temp ep");
    add_basic_info(temp_ep);
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, temp_ep), TAG, "add temp ep");

    ESP_RETURN_ON_ERROR(ezb_af_device_desc_register(dev_desc), TAG, "register device");

    ezb_zcl_core_action_handler_register(core_action_handler);
    ezb_zcl_raw_command_handler_register(raw_command_handler);
    log_simple_descriptors();
    return ESP_OK;
}

static esp_err_t start_temperature_sensing(void)
{
    temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    ESP_RETURN_ON_ERROR(temperature_sensor_install(&tsens_cfg, &s_temp_sensor), TAG, "tsens install");
    ESP_RETURN_ON_ERROR(temperature_sensor_enable(s_temp_sensor), TAG, "tsens enable");

    const esp_timer_create_args_t args = {
        .callback = temp_timer_cb,
        .name = "zb_temp",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_temp_timer), TAG, "tsens timer create");
    return esp_timer_start_periodic(s_temp_timer, TEMP_REPORT_PERIOD_US);
}

static void zb_main_task(void *arg)
{
    (void)arg;

    esp_zigbee_config_t config = ESP_ZIGBEE_ZR_CONFIG();
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));

    ESP_ERROR_CHECK(create_device());

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    ESP_ERROR_CHECK(start_temperature_sensing());
    (void)esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void zb_app_start(void)
{
    ESP_ERROR_CHECK(button_init(button_event_handler, NULL));
    if (xTaskCreate(zb_main_task, "zb_main", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create Zigbee task");
    }
}
