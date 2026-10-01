#include "zigbee/zigbee_command.h"

#include "ha_model/ha_entities.h"

static sys_error_t zigbee_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_ZIGBEE, code);
}

/*
 * Отказ здесь означает «Domain не писал COMMAND_SENT»: команда не ушла исполнителю
 * (docs/domain/COMMANDS.md:31-32). Поэтому проверка выполняется в executor'е, а не в
 * задаче отправки: ошибка должна дойти до вызывающего синхронно.
 */
sys_error_t zigbee_command_check(domain_t *domain, const ha_zb_command_t *command)
{
    if (domain == NULL || command == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (command->device_uid == 0) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (command->args_len > HA_ZB_COMMAND_ARGS_MAX) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    /* Отправлять некуда: устройство ещё не названо Zigbee, значит адресат неизвестен. */
    ha_device_record_t record = {0};
    return domain_entity_get(domain, (domain_entity_t)HA_ENTITY_DEVICE, &command->device_uid,
                             &record);
}
