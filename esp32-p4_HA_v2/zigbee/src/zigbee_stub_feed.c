#include "zigbee/zigbee.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "zigbee/zigbee_diag.h"
#include "zigbee/zigbee_topology.h"

/*
 * Заглушка источников: радио и шлюза пока нет, UI и Automation тоже. Задача поставляет
 * те же репорты, какие пришли бы от устройств, и постит команды от лица UI. Удалить при
 * появлении транспорта и потребителей.
 */

#define FEED_PERIOD_MS 3000
#define FEED_TASK_STACK 2048
#define FEED_TASK_PRIORITY 4
#define STUB_ENDPOINT 1
#define COMMANDS_EVERY_N_REPORTS 3

static const ha_device_uid_t STUB_UID = 0x00124B000A1B2C3Dull;

/* Последовательность подобрана так, чтобы каждый кадр менял состояние: иначе факта нет. */
static const zigbee_report_t SEQUENCE[] = {
    {.device_uid = STUB_UID,
     .cluster_id = HA_ZB_CLUSTER_ON_OFF,
     .attr_id = HA_ZB_ATTR_ON_OFF_ON_OFF,
     .endpoint = STUB_ENDPOINT,
     .zcl_type = HA_ZB_TYPE_BOOL,
     .raw = 1},
    {.device_uid = STUB_UID,
     .cluster_id = HA_ZB_CLUSTER_LEVEL_CONTROL,
     .attr_id = HA_ZB_ATTR_LEVEL_CURRENT_LEVEL,
     .endpoint = STUB_ENDPOINT,
     .zcl_type = HA_ZB_TYPE_UINT8,
     .raw = 127},
    {.device_uid = STUB_UID,
     .cluster_id = HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
     .attr_id = HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE,
     .endpoint = STUB_ENDPOINT,
     .zcl_type = HA_ZB_TYPE_INT16,
     .raw = 2153},
    {.device_uid = STUB_UID,
     .cluster_id = HA_ZB_CLUSTER_ON_OFF,
     .attr_id = HA_ZB_ATTR_ON_OFF_ON_OFF,
     .endpoint = STUB_ENDPOINT,
     .zcl_type = HA_ZB_TYPE_BOOL,
     .raw = 0},
    {.device_uid = STUB_UID,
     .cluster_id = HA_ZB_CLUSTER_LEVEL_CONTROL,
     .attr_id = HA_ZB_ATTR_LEVEL_CURRENT_LEVEL,
     .endpoint = STUB_ENDPOINT,
     .zcl_type = HA_ZB_TYPE_UINT8,
     .raw = 40},
    {.device_uid = STUB_UID,
     .cluster_id = HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT,
     .attr_id = HA_ZB_ATTR_TEMPERATURE_MEASURED_VALUE,
     .endpoint = STUB_ENDPOINT,
     .zcl_type = HA_ZB_TYPE_INT16,
     .raw = 2260},
};

static void submit_command(domain_t *domain, bool on)
{
    ha_zb_command_t command = {0};
    command.device_uid = STUB_UID;
    command.dst_endpoint = STUB_ENDPOINT;
    command.cluster_id = HA_ZB_CLUSTER_ON_OFF;
    command.command_id = on ? HA_ZB_CMD_ON_OFF_ON : HA_ZB_CMD_ON_OFF_OFF;
    command.args_len = 0;

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    meta.value.type = (uint8_t)DOMAIN_VALUE_U32;
    meta.value.v.u32 = on ? 1u : 0u;

    /* Команда адресована устройству: подписчик относит COMMAND_SENT к нему по ключу. */
    const domain_fact_target_t target = {.entity = (domain_entity_t)HA_ENTITY_DEVICE,
                                         .key = &STUB_UID};

    /* Отказ фиксирует сервис; команда для вызывающего — fire-and-forget. */
    (void)domain_post(domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &target, &meta);
}

/* Топология приходит из Simple Descriptor один раз, до репортов. */
static void publish_topology(domain_t *domain)
{
    static const ha_cluster_entry_t clusters[] = {
        {.cluster_id = HA_ZB_CLUSTER_ON_OFF, .role = HA_ZB_ROLE_SERVER},
        {.cluster_id = HA_ZB_CLUSTER_LEVEL_CONTROL, .role = HA_ZB_ROLE_SERVER},
        {.cluster_id = HA_ZB_CLUSTER_TEMPERATURE_MEASUREMENT, .role = HA_ZB_ROLE_SERVER},
    };

    const zigbee_endpoint_desc_t endpoint = {
        .device_uid = STUB_UID,
        .endpoint = STUB_ENDPOINT,
        .profile_id = HA_ZB_PROFILE_HA,
        .device_id = 0x0101u, /* dimmable light — условное устройство заглушки */
        .cluster_count = (uint8_t)(sizeof(clusters) / sizeof(clusters[0])),
        .clusters = clusters,
    };

    bool changed = false;
    const sys_error_t err = zigbee_topology_apply(domain, &endpoint, &changed);
    if (sys_failed(err)) {
        zigbee_diag_record(ZIGBEE_DIAG_TOPOLOGY, err);
    }
}

static void feed_task(void *arg)
{
    domain_t *domain = (domain_t *)arg;

    publish_topology(domain);

    size_t next = 0;
    size_t tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FEED_PERIOD_MS));

        /* Отказ сервис фиксирует сам в своём канале диагностики — здесь он не дублируется. */
        (void)zigbee_submit_report(&SEQUENCE[next]);

        next = (next + 1) % (sizeof(SEQUENCE) / sizeof(SEQUENCE[0]));

        if (++tick % COMMANDS_EVERY_N_REPORTS == 0) {
            submit_command(domain, (tick / COMMANDS_EVERY_N_REPORTS) % 2 == 0);
        }
    }
}

sys_error_t zigbee_stub_feed_start(domain_t *domain)
{
    if (domain == NULL) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_ARG);
    }
    if (xTaskCreate(feed_task, "zigbee_feed", FEED_TASK_STACK, domain, FEED_TASK_PRIORITY, NULL) !=
        pdPASS) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
