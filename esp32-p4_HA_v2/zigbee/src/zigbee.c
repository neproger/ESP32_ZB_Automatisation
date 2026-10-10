#include "zigbee/zigbee.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "zigbee/zigbee_command.h"
#include "zigbee/zigbee_diag.h"
#include "device/device_service.h"
#include "ha_model/ha_capabilities.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_state.h"
#include "zigbee/zigbee_entity_binding.h"
#include "zigbee/zigbee_entity_registry.h"
#include "zigbee/zigbee_radio.h"
#include "zigbee/zigbee_state.h"
#include "semantics/semantics.h"

/*
 * Задача сервиса — единственный, кто говорит с Domain от лица Zigbee-мира: приём
 * репорта и запись состояния идут здесь, а не в контексте радио и не в контексте
 * Dispatcher'а (docs/domain/DOMAIN_API.md §9).
 *
 * Команда приходит иначе: executor вызывается синхронно в контексте вызывающего,
 * поэтому он только проверяет адресата и ставит команду в очередь — отправка идёт из
 * этой же задачи (docs/domain/COMMANDS.md:112-114).
 */

#define ZIGBEE_REPORT_QUEUE_LENGTH 32
#define ZIGBEE_COMMAND_QUEUE_LENGTH 8
#define ZIGBEE_INTERVIEW_QUEUE_LENGTH 4
#define ZIGBEE_EVENT_QUEUE_LENGTH 8
#define ZIGBEE_LEAVE_QUEUE_LENGTH 8
#define ZIGBEE_REMOVE_QUEUE_LENGTH 8
#define ZIGBEE_PERMIT_QUEUE_LENGTH 4
#define ZIGBEE_TASK_STACK 4096
#define ZIGBEE_TASK_PRIORITY 5

static const char *TAG = "zigbee";

static domain_t *s_domain;
static QueueHandle_t s_reports;
static QueueHandle_t s_commands;
static QueueHandle_t s_interviews;
static QueueHandle_t s_events;
static QueueHandle_t s_leaves;
static QueueHandle_t s_remove_requests;
static QueueHandle_t s_permit_requests;
static QueueSetHandle_t s_inbox;

static sys_error_t zigbee_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_ZIGBEE, code);
}

/* Отправка идёт из задачи сервиса: в чужом контексте трогать радио нельзя. */
static void zigbee_send(const ha_zb_command_t *command)
{
    const sys_error_t err = zigbee_radio_send(command);
    if (sys_failed(err)) {
        ESP_LOGW(TAG, "command not sent: uid=%llx ep=%u cluster=%04x cmd=%02x err=%u",
                 (unsigned long long)command->device_uid, (unsigned)command->dst_endpoint,
                 (unsigned)command->cluster_id, (unsigned)command->command_id,
                 (unsigned)err.code);
        zigbee_diag_record(ZIGBEE_DIAG_SEND, err);
    }
}

/* Проверка и постановка физической команды в очередь отправки. */
static sys_error_t zigbee_enqueue_command(const ha_zb_command_t *command)
{
    const sys_error_t check = zigbee_command_check(s_domain, command);
    if (sys_failed(check)) {
        zigbee_diag_record(ZIGBEE_DIAG_COMMAND, check);
        return check;
    }

    if (s_commands == NULL || xQueueSend(s_commands, command, 0) != pdTRUE) {
        const sys_error_t busy = zigbee_fail(SYS_CODE_BUSY);
        zigbee_diag_record(ZIGBEE_DIAG_COMMAND, busy);
        return busy;
    }
    return SYS_OK;
}

static sys_error_t zigbee_execute(domain_command_t type, const void *args, size_t args_size,
                                  void *ctx)
{
    (void)type;
    (void)ctx;

    if (args == NULL || args_size != sizeof(ha_zb_command_t)) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    return zigbee_enqueue_command((const ha_zb_command_t *)args);
}

/*
 * A3.2: семантическая команда по entity. Adapter резолвит entity→(uid, endpoint) по
 * binding, кодировку физической команды делает мост; потребитель транспорта не знает.
 */
static sys_error_t zigbee_property_execute(domain_command_t type, const void *args, size_t args_size,
                                           void *ctx)
{
    (void)type;
    (void)ctx;
    if (args == NULL || args_size != sizeof(ha_property_command_t)) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    const ha_property_command_t *request = (const ha_property_command_t *)args;

    ha_device_uid_t uid = 0;
    uint8_t endpoint = 0;
    if (!zigbee_entity_physical(request->entity, &uid, &endpoint)) {
        return zigbee_fail(SYS_CODE_NOT_FOUND);
    }

    const ha_zb_state_key_t target = {.device_uid = uid, .endpoint = endpoint};
    ha_zb_command_t command = {0};
    if (!semantics_build_command(&target, request->property, request->action, &request->value,
                                 &command)) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    return zigbee_enqueue_command(&command);
}

/*
 * Удаление из локального Zigbee state: пометка (переживёт reboot; при отсутствии в сети
 * leave уйдёт на announce) + leave-запрос немедленно. Успех = «удалено из состояния
 * адаптера/сети», без ожидания подтверждения от физического устройства.
 */
static sys_error_t zigbee_request_remove(ha_device_uid_t uid)
{
    const ha_device_remove_record_t record = {.requested = 1};
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t put = domain_entity_put(
        s_domain, (domain_entity_t)HA_ENTITY_DEVICE_REMOVE, &uid, &record, &meta, &changed);
    if (sys_failed(put)) {
        return put;
    }
    if (s_remove_requests == NULL || xQueueSend(s_remove_requests, &uid, 0) != pdTRUE) {
        return zigbee_fail(SYS_CODE_BUSY);
    }
    return SYS_OK;
}

static sys_error_t zigbee_remove_execute(domain_command_t type, const void *args, size_t args_size,
                                         void *ctx)
{
    (void)type;
    (void)ctx;
    if (args == NULL || args_size != sizeof(ha_device_uid_t)) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    return zigbee_request_remove(*(const ha_device_uid_t *)args);
}

/* Adapter remover (Шаг B, B7): native_id (hex EUI64) → uid → удалить из локального Zigbee state. */
static sys_error_t zigbee_device_remover(const char *native_id, void *ctx)
{
    (void)ctx;
    ha_device_uid_t uid = 0;
    if (!zb_uid_from_native_id(native_id, &uid)) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    return zigbee_request_remove(uid);
}

/* Открыть сеть для подключения новых устройств (по кнопке в UI). */
static sys_error_t zigbee_permit_execute(domain_command_t type, const void *args, size_t args_size,
                                         void *ctx)
{
    (void)type;
    (void)ctx;
    if (args == NULL || args_size != sizeof(uint8_t)) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    const uint8_t seconds = *(const uint8_t *)args;
    if (s_permit_requests == NULL || xQueueSend(s_permit_requests, &seconds, 0) != pdTRUE) {
        return zigbee_fail(SYS_CODE_BUSY);
    }
    return SYS_OK;
}

/*
 * Semantic event адресуется источником-Entity (ключ = entity_id), без device_uid/raw.
 * Command → event_id маппит мост (semantics); Entity резолвит binding.
 */
static void zigbee_publish_entity_event(ha_entity_id_t source_entity, ha_event_id_t event_id)
{
    if (source_entity == HA_ENTITY_ID_NONE) {
        return;
    }
    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_ENTITY,
        .key = &source_entity,
    };
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    meta.value.type = (uint8_t)DOMAIN_VALUE_ENUM;
    meta.value.v.u32 = (uint32_t)event_id;
    const uint8_t payload = (uint8_t)event_id;
    domain_payload_ref_t ref = 0;
    (void)domain_payload_put(s_domain, &target, &meta, &payload, sizeof(payload), &ref);
}

static void zigbee_event_publish(const zigbee_event_t *event)
{
    ha_zb_event_t physical = {0};
    physical.device_uid = event->device_uid;
    physical.endpoint = event->endpoint;
    physical.cluster_id = event->cluster_id;
    physical.command_id = event->command_id;
    physical.payload_len = event->payload_length;
    memcpy(physical.payload, event->payload, event->payload_length);

    ha_device_record_t device = {0};
    const bool has_device = sys_ok(domain_entity_get(
        s_domain, (domain_entity_t)HA_ENTITY_ZB_DIAG_DEVICE, &event->device_uid, &device));
    ha_event_t semantic = {0};
    if (!semantics_decode_event(&physical, has_device ? &device : NULL, &semantic)) {
        return;
    }

    ha_entity_id_t source_entity = HA_ENTITY_ID_NONE;
    if (!zigbee_entity_lookup(event->device_uid, event->endpoint, &source_entity)) {
        return;
    }
    ESP_LOGI(TAG, "event: uid=%llx cluster=%04x cmd=%02x ep=%u -> %u",
             (unsigned long long)event->device_uid, (unsigned)event->cluster_id,
             (unsigned)event->command_id, (unsigned)event->endpoint, (unsigned)semantic.id);
    zigbee_publish_entity_event(source_entity, semantic.id);
}

/*
 * Canonical property state: адресуется (entity_id, property_id). Публикуем, только если
 * endpoint уже имеет logical Entity (после интервью) и атрибут есть в семантическом
 * словаре; иначе смысл не выдумываем (raw остаётся только диагностической записью).
 */
static void zigbee_publish_property_state(const zigbee_report_t *report)
{
    ha_entity_id_t entity_id = HA_ENTITY_ID_NONE;
    if (!zigbee_entity_lookup(report->device_uid, report->endpoint, &entity_id)) {
        return;
    }

    ha_state_key_t key = {0};
    ha_state_record_t record = {0};
    if (!zigbee_property_state_from_report(report, entity_id, &key, &record)) {
        return;
    }

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE,
                                              &key, &record, &meta, &changed);
    if (sys_failed(err)) {
        ESP_LOGW(TAG, "property state not published: uid=%llx ep=%u prop=%u err=%u",
                 (unsigned long long)report->device_uid, (unsigned)report->endpoint,
                 (unsigned)key.property, (unsigned)err.code);
    }
}

/*
 * A6.1b: canonical capabilities Entity из кластеров endpoint'а (через мост), по entity_id.
 * Cluster/attr в записи не сохраняются — web читает properties/actions напрямую.
 */
static void zigbee_publish_capabilities(ha_device_uid_t uid, uint8_t endpoint,
                                        ha_entity_id_t entity_id)
{
    const ha_endpoint_key_t ep_key = {.device_uid = uid, .endpoint = endpoint};
    ha_endpoint_record_t ep_record = {0};
    if (sys_failed(
            domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_ENDPOINT, &ep_key, &ep_record))) {
        return;
    }

    ha_entity_capabilities_record_t caps = {0};
    if (!semantics_endpoint_capabilities(&ep_record, &caps)) {
        return;
    }

    const ha_entity_key_t key = {.id = entity_id};
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_CAPABILITIES,
                                              &key, &caps, &meta, &changed);
    if (sys_failed(err)) {
        ESP_LOGW(TAG, "capabilities not published: id=%llx err=%u",
                 (unsigned long long)entity_id, (unsigned)err.code);
    }
}

static void zigbee_task(void *arg)
{
    (void)arg;

    for (;;) {
        const QueueHandle_t ready = xQueueSelectFromSet(s_inbox, portMAX_DELAY);
        if (ready == NULL) {
            continue;
        }

        if (ready == s_reports) {
            zigbee_report_t report = {0};
            if (xQueueReceive(s_reports, &report, 0) != pdTRUE) {
                continue;
            }

            bool changed = false;
            const sys_error_t err = zigbee_state_apply(s_domain, &report, &changed);
            if (sys_failed(err)) {
                ESP_LOGW(TAG, "report dropped: uid=%llx cluster=%04x attr=%04x",
                         (unsigned long long)report.device_uid, (unsigned)report.cluster_id,
                         (unsigned)report.attr_id);
                zigbee_diag_record(ZIGBEE_DIAG_REPORT, err);
            } else {
                zigbee_publish_property_state(&report);
            }
            continue;
        }

        if (ready == s_commands) {
            ha_zb_command_t command = {0};
            if (xQueueReceive(s_commands, &command, 0) == pdTRUE) {
                zigbee_send(&command);
            }
            continue;
        }

        if (ready == s_interviews) {
            zigbee_interview_result_t result = {0};
            if (xQueueReceive(s_interviews, &result, 0) == pdTRUE) {
                const sys_error_t err = zigbee_interview_apply(s_domain, &result);
                if (sys_failed(err)) {
                    ESP_LOGW(TAG, "interview not applied: uid=%llx err=%u",
                             (unsigned long long)result.uid, (unsigned)err.code);
                    zigbee_diag_record(ZIGBEE_DIAG_TOPOLOGY, err);
                } else {
                    ESP_LOGI(TAG, "interview applied: uid=%llx endpoints=%u",
                             (unsigned long long)result.uid, (unsigned)result.endpoint_count);

                    /*
                     * B4': Zigbee описывает нормализованный Device (root) и подаёт его в Device
                     * service; ownership Entity задаёт Device service, не adapter. Пока —
                     * плоский root без child Device.
                     */
                    ha_device_id_t device_id = HA_DEVICE_ID_NONE;
                    ha_device_desc_t desc = {0};
                    desc.source = HA_SOURCE_ZIGBEE;
                    (void)zb_device_native_id(result.uid, desc.native_id, sizeof(desc.native_id));
                    ha_device_record_t device = {0};
                    if (sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_ZB_DIAG_DEVICE,
                                                 &result.uid, &device))) {
                        size_t n = strlen(device.name);
                        if (n >= sizeof(desc.name)) {
                            n = sizeof(desc.name) - 1;
                        }
                        memcpy(desc.name, device.name, n);
                    }
                    if (sys_failed(device_register(s_domain, (uint8_t)DOMAIN_SOURCE_ZIGBEE, &desc,
                                                   &device_id))) {
                        ESP_LOGW(TAG, "device register failed uid=%llx",
                                 (unsigned long long)result.uid);
                    } else {
                        ESP_LOGI(TAG, "device: uid=%llx id=%llx native=%s",
                                 (unsigned long long)result.uid, (unsigned long long)device_id,
                                 desc.native_id);
                    }

                    /* A0.4/A6.1b/B4': endpoint → логическая Entity + ownership + capabilities. */
                    for (uint8_t i = 0; i < result.endpoint_count; i++) {
                        ha_entity_id_t entity_id = HA_ENTITY_ID_NONE;
                        const uint8_t endpoint = result.endpoints[i].endpoint;
                        if (sys_ok(zigbee_entity_ensure(s_domain, result.uid, endpoint,
                                                        result.endpoint_count, &entity_id))) {
                            if (device_id != HA_DEVICE_ID_NONE) {
                                const sys_error_t bind = device_bind_entity(
                                    s_domain, (uint8_t)DOMAIN_SOURCE_ZIGBEE, device_id, entity_id);
                                ESP_LOGI(TAG, "entity->device: uid=%llx ep=%u entity=%llx device=%llx ok=%d",
                                         (unsigned long long)result.uid, (unsigned)endpoint,
                                         (unsigned long long)entity_id,
                                         (unsigned long long)device_id, (int)sys_ok(bind));
                            }
                            zigbee_publish_capabilities(result.uid, endpoint, entity_id);
                        }
                    }
                }
            }
            continue;
        }

        if (ready == s_events) {
            zigbee_event_t event = {0};
            if (xQueueReceive(s_events, &event, 0) == pdTRUE) {
                zigbee_event_publish(&event);
            }
            continue;
        }

        if (ready == s_leaves) {
            ha_device_uid_t uid = 0;
            if (xQueueReceive(s_leaves, &uid, 0) == pdTRUE) {
                const sys_error_t err = zigbee_device_remove(s_domain, uid);
                if (sys_failed(err)) {
                    ESP_LOGW(TAG, "device not removed: uid=%llx err=%u",
                             (unsigned long long)uid, (unsigned)err.code);
                    zigbee_diag_record(ZIGBEE_DIAG_TOPOLOGY, err);
                } else {
                    ESP_LOGI(TAG, "device removed: uid=%llx", (unsigned long long)uid);
                    zigbee_entity_forget(uid); /* adapter-private bindings больше не нужны */
                    /* Устройство ушло — снимаем и пометку на удаление. */
                    domain_fact_meta_t meta = {0};
                    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
                    (void)domain_entity_remove(
                        s_domain, (domain_entity_t)HA_ENTITY_DEVICE_REMOVE, &uid, &meta);
                }
            }
            continue;
        }

        if (ready == s_remove_requests) {
            ha_device_uid_t uid = 0;
            if (xQueueReceive(s_remove_requests, &uid, 0) == pdTRUE) {
                const sys_error_t err = zigbee_radio_request_leave(uid);
                if (sys_failed(err)) {
                    ESP_LOGI(TAG, "leave not sent: uid=%llx err=%u (ждём announce)",
                             (unsigned long long)uid, (unsigned)err.code);
                }
            }
            continue;
        }

        if (ready == s_permit_requests) {
            uint8_t seconds = 0;
            if (xQueueReceive(s_permit_requests, &seconds, 0) == pdTRUE) {
                const sys_error_t err = zigbee_radio_open_network(seconds);
                if (sys_failed(err)) {
                    ESP_LOGW(TAG, "network not opened: err=%u", (unsigned)err.code);
                }
            }
        }
    }
}

sys_error_t zigbee_start(domain_t *domain)
{
    if (domain == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    s_domain = domain;
    s_reports = xQueueCreate(ZIGBEE_REPORT_QUEUE_LENGTH, sizeof(zigbee_report_t));
    s_commands = xQueueCreate(ZIGBEE_COMMAND_QUEUE_LENGTH, sizeof(ha_zb_command_t));
    s_interviews = xQueueCreate(ZIGBEE_INTERVIEW_QUEUE_LENGTH, sizeof(zigbee_interview_result_t));
    s_events = xQueueCreate(ZIGBEE_EVENT_QUEUE_LENGTH, sizeof(zigbee_event_t));
    s_leaves = xQueueCreate(ZIGBEE_LEAVE_QUEUE_LENGTH, sizeof(ha_device_uid_t));
    s_remove_requests = xQueueCreate(ZIGBEE_REMOVE_QUEUE_LENGTH, sizeof(ha_device_uid_t));
    s_permit_requests = xQueueCreate(ZIGBEE_PERMIT_QUEUE_LENGTH, sizeof(uint8_t));
    s_inbox = xQueueCreateSet(ZIGBEE_REPORT_QUEUE_LENGTH + ZIGBEE_COMMAND_QUEUE_LENGTH +
                              ZIGBEE_INTERVIEW_QUEUE_LENGTH + ZIGBEE_EVENT_QUEUE_LENGTH +
                              ZIGBEE_LEAVE_QUEUE_LENGTH + ZIGBEE_REMOVE_QUEUE_LENGTH +
                              ZIGBEE_PERMIT_QUEUE_LENGTH);
    if (s_reports == NULL || s_commands == NULL || s_interviews == NULL || s_events == NULL ||
        s_leaves == NULL || s_remove_requests == NULL || s_permit_requests == NULL ||
        s_inbox == NULL) {
        return zigbee_fail(SYS_CODE_NO_MEM);
    }
    if (xQueueAddToSet(s_reports, s_inbox) != pdPASS ||
        xQueueAddToSet(s_commands, s_inbox) != pdPASS ||
        xQueueAddToSet(s_interviews, s_inbox) != pdPASS ||
        xQueueAddToSet(s_events, s_inbox) != pdPASS ||
        xQueueAddToSet(s_leaves, s_inbox) != pdPASS ||
        xQueueAddToSet(s_remove_requests, s_inbox) != pdPASS ||
        xQueueAddToSet(s_permit_requests, s_inbox) != pdPASS) {
        return zigbee_fail(SYS_CODE_NO_MEM);
    }

    if (xTaskCreate(zigbee_task, "zigbee", ZIGBEE_TASK_STACK, NULL, ZIGBEE_TASK_PRIORITY, NULL) !=
        pdPASS) {
        return zigbee_fail(SYS_CODE_NO_MEM);
    }

    /* Регистрация исполнителя — часть инициализации, а не runtime-механизм. */
    const sys_error_t cluster =
        domain_register_command(domain, HA_CMD_ZIGBEE_CLUSTER, zigbee_execute, NULL);
    domain_register_command(domain, HA_CMD_PROPERTY, zigbee_property_execute, NULL);
    if (sys_failed(cluster)) {
        return cluster;
    }
    const sys_error_t remove =
        domain_register_command(domain, HA_CMD_DEVICE_REMOVE, zigbee_remove_execute, NULL);
    (void)device_register_remover(HA_SOURCE_ZIGBEE, zigbee_device_remover, NULL);
    if (sys_failed(remove)) {
        return remove;
    }
    return domain_register_command(domain, HA_CMD_PERMIT_JOIN, zigbee_permit_execute, NULL);
}

sys_error_t zigbee_submit_report(const zigbee_report_t *report)
{
    if (report == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (s_reports == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_STATE);
    }
    if (xQueueSend(s_reports, report, 0) != pdTRUE) {
        const sys_error_t busy = zigbee_fail(SYS_CODE_BUSY);
        zigbee_diag_record(ZIGBEE_DIAG_REPORT_QUEUE, busy);
        return busy;
    }
    return SYS_OK;
}

sys_error_t zigbee_submit_interview(const zigbee_interview_result_t *result)
{
    if (result == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (s_interviews == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_STATE);
    }
    if (xQueueSend(s_interviews, result, 0) != pdTRUE) {
        return zigbee_fail(SYS_CODE_BUSY);
    }
    return SYS_OK;
}

sys_error_t zigbee_submit_event(const zigbee_event_t *event)
{
    if (event == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (s_events == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_STATE);
    }
    if (xQueueSend(s_events, event, 0) != pdTRUE) {
        return zigbee_fail(SYS_CODE_BUSY);
    }
    return SYS_OK;
}

sys_error_t zigbee_submit_leave(ha_device_uid_t device_uid)
{
    if (s_leaves == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_STATE);
    }
    if (xQueueSend(s_leaves, &device_uid, 0) != pdTRUE) {
        return zigbee_fail(SYS_CODE_BUSY);
    }
    return SYS_OK;
}
