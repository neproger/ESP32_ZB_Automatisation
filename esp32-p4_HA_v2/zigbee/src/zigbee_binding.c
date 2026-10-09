#include "zigbee/zigbee_binding.h"

#include "esp_log.h"
#include "esp_zigbee.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/cluster/color_control_desc.h"
#include "ezbee/zcl/cluster/illuminance_measurement_desc.h"
#include "ezbee/zcl/cluster/level_desc.h"
#include "ezbee/zcl/cluster/occupancy_sensing_desc.h"
#include "ezbee/zcl/cluster/on_off_desc.h"
#include "ezbee/zcl/cluster/power_config_desc.h"
#include "ezbee/zcl/cluster/rel_humidity_measurement_desc.h"
#include "ezbee/zcl/cluster/temperature_measurement_desc.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "ezbee/zdo/zdo_bind_mgmt.h"

/*
 * Что подписываем и как часто (docs/services/ZIGBEE.md §4). Таблица — единственное
 * место, где перечислены reportable-атрибуты: отсюда же берутся client-кластеры
 * координатора, поэтому списки не расходятся. На один cluster может быть несколько
 * строк — у него несколько reportable-атрибутов (цвет: hue/sat/X/Y/temp); bind
 * делается один раз на кластер, configure_reporting — на каждый атрибут.
 *
 * reportable_change — минимальное изменение значения для аналоговых атрибутов
 * (в единицах ZCL: уровень 0..254, температура 0.01 °C, влажность 0.01 %).
 */

static const char *TAG = "zigbee.binding";

#define COORDINATOR_ENDPOINT 1

typedef struct {
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t attr_type;
    uint16_t min_interval;
    uint16_t max_interval;
    int32_t reportable_change;
} report_rule_t;

static const report_rule_t REPORT_RULES[] = {
    /* Управляемые атрибуты: min_interval=0 — репорт немедленно на изменение
     * (иначе устройство склеивает быстрые изменения и UI отстаёт; v1 работал так же). */
    {HA_ZB_CLUSTER_ON_OFF, HA_ZB_ATTR_ON_OFF_ON_OFF, HA_ZB_TYPE_BOOL, 0, 60, 1},
    {HA_ZB_CLUSTER_LEVEL_CONTROL, HA_ZB_ATTR_LEVEL_CURRENT_LEVEL, HA_ZB_TYPE_UINT8, 0, 60, 1},
    /* Color Control держит независимые атрибуты режимов — по строке на атрибут. */
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_HUE, HA_ZB_TYPE_UINT8, 0, 60, 1},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_SATURATION, HA_ZB_TYPE_UINT8, 0, 60, 1},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_X, HA_ZB_TYPE_UINT16, 0, 60, 1},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_CURRENT_Y, HA_ZB_TYPE_UINT16, 0, 60, 1},
    {HA_ZB_CLUSTER_COLOR_CONTROL, HA_ZB_ATTR_COLOR_COLOR_TEMPERATURE, HA_ZB_TYPE_UINT16, 0, 60, 1},
    {HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT, HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE, HA_ZB_TYPE_INT16,
     10, 60, 100},
    {HA_ZB_CLUSTER_RELATIVE_HUMIDITY, HA_ZB_ATTR_HUMIDITY_MEASURED_VALUE, HA_ZB_TYPE_UINT16, 10,
     60, 100},
    {HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT, HA_ZB_ATTR_ILLUMINANCE_MEASURED_VALUE,
     HA_ZB_TYPE_UINT16, 10, 60, 100},
    {HA_ZB_CLUSTER_OCCUPANCY_SENSING, HA_ZB_ATTR_OCCUPANCY_OCCUPANCY, HA_ZB_TYPE_BITMAP8, 1, 60,
     1},
    {HA_ZB_CLUSTER_POWER_CONFIG, HA_ZB_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING,
     HA_ZB_TYPE_UINT8, 60, 60, 1},
};

#define REPORT_RULES_COUNT (sizeof(REPORT_RULES) / sizeof(REPORT_RULES[0]))

/* Reportable-атрибуты кластера (для чтения текущих значений после интервью). */
uint8_t zigbee_binding_attrs_for(uint16_t cluster_id, uint16_t *out, uint8_t max)
{
    uint8_t n = 0;
    for (size_t i = 0; i < REPORT_RULES_COUNT && n < max; i++) {
        if (REPORT_RULES[i].cluster_id == cluster_id) {
            out[n++] = REPORT_RULES[i].attr_id;
        }
    }
    return n;
}

static ezb_zcl_cluster_desc_t create_client_cluster(uint16_t cluster_id)
{
    switch (cluster_id) {
    case HA_ZB_CLUSTER_ON_OFF:
        return ezb_zcl_on_off_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    case HA_ZB_CLUSTER_LEVEL_CONTROL:
        return ezb_zcl_level_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    case HA_ZB_CLUSTER_COLOR_CONTROL:
        return ezb_zcl_color_control_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    case HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT:
        return ezb_zcl_temperature_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    case HA_ZB_CLUSTER_RELATIVE_HUMIDITY:
        return ezb_zcl_rel_humidity_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    case HA_ZB_CLUSTER_ILLUMINANCE_MEASUREMENT:
        return ezb_zcl_illuminance_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    case HA_ZB_CLUSTER_OCCUPANCY_SENSING:
        return ezb_zcl_occupancy_sensing_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    case HA_ZB_CLUSTER_POWER_CONFIG:
        return ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
    default:
        return NULL;
    }
}

void zigbee_binding_add_client_clusters(ezb_af_ep_desc_t coordinator_endpoint)
{
    for (size_t i = 0; i < REPORT_RULES_COUNT; i++) {
        bool seen = false; /* один client-кластер на cluster_id, даже если строк несколько */
        for (size_t j = 0; j < i; j++) {
            if (REPORT_RULES[j].cluster_id == REPORT_RULES[i].cluster_id) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        ezb_zcl_cluster_desc_t cluster = create_client_cluster(REPORT_RULES[i].cluster_id);
        if (cluster == NULL) {
            continue;
        }
        const ezb_err_t err = ezb_af_endpoint_add_cluster_desc(coordinator_endpoint, cluster);
        if (err != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "client cluster %04x not added: code=0x%x",
                     (unsigned)REPORT_RULES[i].cluster_id, (unsigned)err);
        }
    }
}

static void bind_result(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    const report_rule_t *rule = (const report_rule_t *)user_ctx;
    if (result->error == EZB_ERR_NONE && result->rsp != NULL &&
        result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "bound cluster=%04x", (unsigned)rule->cluster_id);
        return;
    }
    ESP_LOGW(TAG, "bind failed: cluster=%04x error=0x%x status=0x%02x", (unsigned)rule->cluster_id,
             (unsigned)result->error, result->rsp != NULL ? (unsigned)result->rsp->status : 0xFFu);
}

static void bind_cluster(const report_rule_t *rule, ha_device_uid_t uid, uint16_t short_addr,
                         uint8_t endpoint)
{
    ezb_extaddr_t coordinator = {0};
    ezb_nwk_get_extended_address(&coordinator);

    const ezb_zdo_bind_req_t request = {
        .dst_nwk_addr = short_addr,
        .field =
            {
                .src_addr = {.u64 = uid},
                .src_ep = endpoint,
                .cluster_id = rule->cluster_id,
                .dst_addr_mode = EZB_ADDR_MODE_EXT,
                .dst_addr = {.extended_addr = coordinator},
                .dst_ep = COORDINATOR_ENDPOINT,
            },
        .cb = bind_result,
        .user_ctx = (void *)rule,
    };
    if (ezb_zdo_bind_req(&request) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "bind not sent: cluster=%04x ep=%u", (unsigned)rule->cluster_id,
                 (unsigned)endpoint);
    }
}

static void reportable_change_set(ezb_zcl_attr_variable_t *target, uint8_t attr_type, int32_t value)
{
    switch (attr_type) {
    case HA_ZB_TYPE_BOOL:
    case HA_ZB_TYPE_BITMAP8:
    case HA_ZB_TYPE_UINT8:
        target->u8 = (uint8_t)value;
        break;
    case HA_ZB_TYPE_UINT16:
        target->u16 = (uint16_t)value;
        break;
    case HA_ZB_TYPE_UINT32:
        target->u32 = (uint32_t)value;
        break;
    case HA_ZB_TYPE_INT8:
        target->s8 = (int8_t)value;
        break;
    case HA_ZB_TYPE_INT16:
        target->s16 = (int16_t)value;
        break;
    case HA_ZB_TYPE_INT32:
        target->s32 = value;
        break;
    default:
        break;
    }
}

static void configure_reporting(const report_rule_t *rule, uint16_t short_addr, uint8_t endpoint)
{
    ezb_zcl_attr_variable_t change = {0};
    reportable_change_set(&change, rule->attr_type, rule->reportable_change);

    ezb_zcl_config_report_record_t record = {
        .direction = EZB_ZCL_REPORTING_SEND,
        .attr_id = rule->attr_id,
        .client =
            {
                .attr_type = rule->attr_type,
                .min_interval = rule->min_interval,
                .max_interval = rule->max_interval,
                .reportable_change = change,
            },
    };
    const ezb_zcl_config_report_cmd_t request = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .src_ep = COORDINATOR_ENDPOINT,
                .cluster_id = rule->cluster_id,
            },
        .payload.record_number = 1,
        .payload.record_field = &record,
    };
    if (ezb_zcl_config_report_cmd_req(&request) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "reporting not configured: cluster=%04x ep=%u", (unsigned)rule->cluster_id,
                 (unsigned)endpoint);
    }
}

void zigbee_binding_apply(ha_device_uid_t uid, uint16_t short_addr,
                          const zigbee_interview_endpoint_t *endpoints, uint8_t endpoint_count)
{
    if (endpoints == NULL) {
        return;
    }

    for (uint8_t e = 0; e < endpoint_count; e++) {
        const zigbee_interview_endpoint_t *endpoint = &endpoints[e];
        for (uint8_t c = 0; c < endpoint->cluster_count; c++) {
            if (endpoint->clusters[c].role != HA_ZB_ROLE_SERVER) {
                continue;
            }
            const uint16_t cluster_id = endpoint->clusters[c].cluster_id;
            bool bound = false; /* bind — на кластер; reporting — на каждый атрибут */
            for (size_t r = 0; r < REPORT_RULES_COUNT; r++) {
                if (REPORT_RULES[r].cluster_id != cluster_id) {
                    continue;
                }
                if (!bound) {
                    bind_cluster(&REPORT_RULES[r], uid, short_addr, endpoint->endpoint);
                    bound = true;
                }
                configure_reporting(&REPORT_RULES[r], short_addr, endpoint->endpoint);
            }
        }
    }
}
