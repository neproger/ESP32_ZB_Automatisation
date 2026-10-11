#include "web/web_commands.h"

#include <string.h>

#include "device/device_service.h"
#include "esp_log.h"
#include "ha_model/ha_automation.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_logical_device.h"
#include "ha_model/ha_rule.h"
#include "ha_model/ha_state.h"
#include "sys/sys_error.h"
#include "web/web_proto.h"

static const char *TAG = "web";

static domain_t *s_domain;

/* --- args = canonical struct verbatim → Domain -------------------------- */

static uint16_t post_property_command(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_property_command_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err = domain_post(s_domain, HA_CMD_PROPERTY, args, len, NULL, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t post_entity_rename(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_entity_rename_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err = domain_post(s_domain, HA_CMD_ENTITY_RENAME, args, len, NULL, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Rename Logical Device: ha_device_id_t + char name[32] (verbatim, через Device service). */
static uint16_t do_device_rename(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_id_t) + HA_LOGICAL_DEVICE_NAME_MAX) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_id_t device_id = 0;
    memcpy(&device_id, args, sizeof(device_id));
    char name[HA_LOGICAL_DEVICE_NAME_MAX + 1] = {0};
    memcpy(name, args + sizeof(device_id), HA_LOGICAL_DEVICE_NAME_MAX);
    const sys_error_t err = device_rename(s_domain, (uint8_t)DOMAIN_SOURCE_UI, device_id, name);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Remove Logical Device: ha_device_id_t (adapter remove + cascade через Device service). */
static uint16_t do_logical_device_remove(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_id_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_id_t device_id = 0;
    memcpy(&device_id, args, sizeof(device_id));
    const sys_error_t err = device_remove(s_domain, (uint8_t)DOMAIN_SOURCE_UI, device_id);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t put_entity(const uint8_t *args, size_t len, ha_entity_t type, uint16_t key_size,
                           uint16_t rec_size)
{
    if (len != (size_t)key_size + rec_size) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err =
        domain_entity_put(s_domain, (domain_entity_t)type, args, args + key_size, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t remove_entity(const uint8_t *args, size_t len, ha_entity_t type,
                              uint16_t key_size)
{
    if (len != key_size) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err = domain_entity_remove(s_domain, (domain_entity_t)type, args, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t do_automation_put(const uint8_t *args, size_t len)
{
    return put_entity(args, len, HA_ENTITY_AUTOMATION, sizeof(ha_automation_key_t),
                      sizeof(ha_rule_t));
}

static uint16_t do_automation_remove(const uint8_t *args, size_t len)
{
    return remove_entity(args, len, HA_ENTITY_AUTOMATION, sizeof(ha_automation_key_t));
}

static uint16_t do_group_put(const uint8_t *args, size_t len)
{
    return put_entity(args, len, HA_ENTITY_GROUP, sizeof(ha_group_key_t),
                      sizeof(ha_group_record_t));
}

static uint16_t do_group_remove(const uint8_t *args, size_t len)
{
    return remove_entity(args, len, HA_ENTITY_GROUP, sizeof(ha_group_key_t));
}

static uint16_t do_group_item_put(const uint8_t *args, size_t len)
{
    return put_entity(args, len, HA_ENTITY_GROUP_ITEM, sizeof(ha_group_item_key_t),
                      sizeof(ha_group_item_record_t));
}

static uint16_t do_group_item_remove(const uint8_t *args, size_t len)
{
    return remove_entity(args, len, HA_ENTITY_GROUP_ITEM, sizeof(ha_group_item_key_t));
}

static uint16_t do_location_put(const uint8_t *args, size_t len)
{
    return put_entity(args, len, HA_ENTITY_LOCATION, sizeof(ha_device_id_t),
                      sizeof(ha_location_record_t));
}

static uint16_t do_discovery(const uint8_t *args, size_t len)
{
    if (len != 1) {
        return SYS_CODE_INVALID_SIZE;
    }
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err = domain_post(s_domain, HA_CMD_DISCOVERY, args, len, NULL, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

void web_commands_handle(uint8_t cmd, const uint8_t *args, size_t len)
{
    uint16_t status = (uint16_t)SYS_CODE_INVALID_ARG;
    switch (cmd) {
    case WEB_CMD_ENTITY_RENAME:
        status = post_entity_rename(args, len);
        break;
    case WEB_CMD_DEVICE_RENAME:
        status = do_device_rename(args, len);
        break;
    case WEB_CMD_LOGICAL_DEVICE_REMOVE:
        status = do_logical_device_remove(args, len);
        break;
    case WEB_CMD_PROPERTY_COMMAND:
        status = post_property_command(args, len);
        break;
    case WEB_CMD_AUTOMATION_PUT:
        status = do_automation_put(args, len);
        break;
    case WEB_CMD_AUTOMATION_REMOVE:
        status = do_automation_remove(args, len);
        break;
    case WEB_CMD_DISCOVERY:
        status = do_discovery(args, len);
        break;
    case WEB_CMD_GROUP_PUT:
        status = do_group_put(args, len);
        break;
    case WEB_CMD_GROUP_REMOVE:
        status = do_group_remove(args, len);
        break;
    case WEB_CMD_GROUP_ITEM_PUT:
        status = do_group_item_put(args, len);
        break;
    case WEB_CMD_GROUP_ITEM_REMOVE:
        status = do_group_item_remove(args, len);
        break;
    case WEB_CMD_LOCATION_PUT:
        status = do_location_put(args, len);
        break;
    default:
        break;
    }
    if (status != (uint16_t)SYS_CODE_OK) {
        ESP_LOGW(TAG, "command %u rejected: code=%u", (unsigned)cmd, (unsigned)status);
    }
}

void web_commands_init(domain_t *domain)
{
    s_domain = domain;
}
