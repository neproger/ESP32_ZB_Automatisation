#include "zigbee/zigbee.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "zigbee/zigbee_command.h"
#include "zigbee/zigbee_diag.h"
#include "zigbee/zigbee_radio.h"
#include "zigbee/zigbee_state.h"

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
#define ZIGBEE_TASK_STACK 4096
#define ZIGBEE_TASK_PRIORITY 5

static const char *TAG = "zigbee";

static domain_t *s_domain;
static QueueHandle_t s_reports;
static QueueHandle_t s_commands;
static QueueHandle_t s_interviews;
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

static sys_error_t zigbee_execute(domain_command_t type, const void *args, size_t args_size,
                                  void *ctx)
{
    (void)type;
    (void)ctx;

    if (args == NULL || args_size != sizeof(ha_zb_command_t)) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    const ha_zb_command_t *command = (const ha_zb_command_t *)args;
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
    s_inbox = xQueueCreateSet(ZIGBEE_REPORT_QUEUE_LENGTH + ZIGBEE_COMMAND_QUEUE_LENGTH +
                              ZIGBEE_INTERVIEW_QUEUE_LENGTH);
    if (s_reports == NULL || s_commands == NULL || s_interviews == NULL || s_inbox == NULL) {
        return zigbee_fail(SYS_CODE_NO_MEM);
    }
    if (xQueueAddToSet(s_reports, s_inbox) != pdPASS ||
        xQueueAddToSet(s_commands, s_inbox) != pdPASS ||
        xQueueAddToSet(s_interviews, s_inbox) != pdPASS) {
        return zigbee_fail(SYS_CODE_NO_MEM);
    }

    if (xTaskCreate(zigbee_task, "zigbee", ZIGBEE_TASK_STACK, NULL, ZIGBEE_TASK_PRIORITY, NULL) !=
        pdPASS) {
        return zigbee_fail(SYS_CODE_NO_MEM);
    }

    /* Регистрация исполнителя — часть инициализации, а не runtime-механизм. */
    return domain_register_command(domain, HA_CMD_ZIGBEE_CLUSTER, zigbee_execute, NULL);
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
