#include "zigbee/zigbee_command.h"

#include <stdio.h>
#include <string.h>

#include "zigbee/zigbee_device_store.h"

/*
 * Проверяется то, что сервис может проверить до отправки команды: адресат известен
 * адаптеру и форма аргументов не выходит за лимит (docs/services/ZIGBEE.md §5).
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static const ha_device_uid_t UID = 0x00124B000A1B2C3Dull;

static ha_zb_command_t command_for(ha_device_uid_t uid, uint8_t args_len)
{
    ha_zb_command_t command = {0};
    command.device_uid = uid;
    command.dst_endpoint = 1;
    command.cluster_id = HA_ZB_CLUSTER_ON_OFF;
    command.command_id = HA_ZB_CMD_ON_OFF_ON;
    command.args_len = args_len;
    return command;
}

static void test_unknown_device_is_rejected(void)
{
    const ha_zb_command_t command = command_for(UID, 0);
    CHECK(sys_is(zigbee_command_check(&command), SYS_CODE_NOT_FOUND));
}

static void test_known_device_is_accepted(void)
{
    bool changed = false;
    CHECK(sys_ok(zigbee_device_set_model(UID, "TS0001", &changed)));

    const ha_zb_command_t command = command_for(UID, 0);
    CHECK(sys_ok(zigbee_command_check(&command)));

    zigbee_device_remove(UID);
}

static void test_malformed_command_is_rejected(void)
{
    CHECK(sys_is(zigbee_command_check(NULL), SYS_CODE_INVALID_ARG));

    const ha_zb_command_t no_uid = command_for(0, 0);
    CHECK(sys_is(zigbee_command_check(&no_uid), SYS_CODE_INVALID_ARG));

    const ha_zb_command_t oversized = command_for(UID, HA_ZB_COMMAND_ARGS_MAX + 1);
    CHECK(sys_is(zigbee_command_check(&oversized), SYS_CODE_INVALID_ARG));
}

int main(void)
{
    test_unknown_device_is_rejected();
    test_known_device_is_accepted();
    test_malformed_command_is_rejected();

    if (g_failures != 0) {
        printf("test_command: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_command: OK\n");
    return 0;
}
