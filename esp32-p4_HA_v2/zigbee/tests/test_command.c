#include "zigbee/zigbee_command.h"

#include <stdio.h>
#include <string.h>

#include "domain/domain.h"
#include "ha_model/ha_entities.h"
#include "mstore_platform.h"
#include "nor_sim.h"
#include "nor_sim_device.h"

/*
 * Проверяется то, что сервис может проверить до отправки команды: адресат известен
 * системе и форма аргументов не выходит за лимит (docs/services/ZIGBEE.md §5).
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define SIM_SIZE (64u * 1024u)
#define SIM_ERASE_SIZE 4096u

static const ha_device_uid_t UID = 0x00124B000A1B2C3Dull;

static void flash_on(mstore_nor_sim_t **out_sim, nor_sim_device_t *device)
{
    *out_sim = mstore_nor_sim_create(SIM_SIZE, SIM_ERASE_SIZE);
    CHECK(*out_sim != NULL);
    nor_sim_device_init(device, *out_sim);
    mstore_platform_flash_set_device(&device->base);
}

static void flash_off(mstore_nor_sim_t *sim)
{
    mstore_platform_flash_set_device(NULL);
    mstore_nor_sim_destroy(sim);
}

static void register_types(domain_t *domain)
{
    domain_entity_desc_t device = {0};
    device.type = (domain_entity_t)HA_ENTITY_DEVICE;
    device.key_size = sizeof(ha_device_uid_t);
    device.payload_size = sizeof(ha_device_record_t);
    device.capacity = 32;
    device.backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH;
    device.persist_key = "device";
    CHECK(sys_ok(domain_register_entity(domain, &device)));

    domain_entity_desc_t state = {0};
    state.type = (domain_entity_t)HA_ENTITY_STATE;
    state.key_size = sizeof(ha_zb_state_key_t);
    state.payload_size = sizeof(ha_zb_state_record_t);
    state.capacity = 256;
    state.backing = DOMAIN_BACKING_RAM;
    state.persist_key = NULL;
    CHECK(sys_ok(domain_register_entity(domain, &state)));
}

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
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 16, 8, 64)));
    register_types(&domain);

    const ha_zb_command_t command = command_for(UID, 0);
    CHECK(sys_is(zigbee_command_check(&domain, &command), SYS_CODE_NOT_FOUND));

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

static void test_known_device_is_accepted(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 16, 8, 64)));
    register_types(&domain);

    ha_device_record_t record = {0};
    bool changed = false;
    CHECK(sys_ok(domain_entity_put(&domain, (domain_entity_t)HA_ENTITY_DEVICE, &UID, &record, NULL,
                                   &changed)));

    const ha_zb_command_t command = command_for(UID, 0);
    CHECK(sys_ok(zigbee_command_check(&domain, &command)));

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
}

static void test_malformed_command_is_rejected(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 16, 8, 64)));
    register_types(&domain);

    CHECK(sys_is(zigbee_command_check(&domain, NULL), SYS_CODE_INVALID_ARG));

    const ha_zb_command_t no_uid = command_for(0, 0);
    CHECK(sys_is(zigbee_command_check(&domain, &no_uid), SYS_CODE_INVALID_ARG));

    const ha_zb_command_t oversized = command_for(UID, HA_ZB_COMMAND_ARGS_MAX + 1);
    CHECK(sys_is(zigbee_command_check(&domain, &oversized), SYS_CODE_INVALID_ARG));

    CHECK(sys_ok(domain_deinit(&domain)));
    flash_off(sim);
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
