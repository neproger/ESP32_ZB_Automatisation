#include "zigbee/zigbee_command.h"

#include "zigbee/zigbee_adapter_ids.h"

#include "zigbee/zigbee_device_store.h"

static sys_error_t zigbee_fail(sys_code_t code)
{
    return sys_error_make(ZIGBEE_SYS_LAYER, code);
}

/*
 * Отказ здесь означает «команда не ушла исполнителю» (docs/domain/COMMANDS.md:31-32).
 * Поэтому проверка выполняется в executor'е, а не в задаче отправки: ошибка должна дойти
 * до вызывающего синхронно.
 */
sys_error_t zigbee_command_check(const ha_zb_command_t *command)
{
    if (command == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (command->device_uid == 0) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (command->args_len > HA_ZB_COMMAND_ARGS_MAX) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    /* Отправлять некуда: устройство ещё не названо Zigbee, значит адресат неизвестен. */
    if (!zigbee_device_has(command->device_uid)) {
        return zigbee_fail(SYS_CODE_NOT_FOUND);
    }
    return SYS_OK;
}
