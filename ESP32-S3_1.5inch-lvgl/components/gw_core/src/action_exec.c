#include "gw_core/action_exec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_proto/gw_proto_types.h"
#include "gw_zigbee/gw_zigbee.h"

static void set_err(char *err, size_t err_size, const char *msg)
{
    if (!err || err_size == 0) {
        return;
    }
    if (!msg) {
        err[0] = '\0';
        return;
    }
    strncpy(err, msg, err_size);
    err[err_size - 1] = '\0';
}

static const char *strtab_at_entry(const gw_automation_entry_t *entry, uint32_t off)
{
    if (!entry) return "";
    if (off == 0) return "";
    if (off >= entry->string_table_size) return "";
    return entry->string_table + off;
}

static esp_err_t exec_device_cluster_cmd(const gw_device_uid_t *uid,
                                         uint8_t endpoint,
                                         uint16_t cluster_id,
                                         uint8_t cmd_id,
                                         uint32_t arg0,
                                         uint32_t arg1,
                                         uint32_t arg2,
                                         char *err,
                                         size_t err_size)
{
    if (!uid || uid->uid[0] == '\0') {
        set_err(err, err_size, "missing uid");
        return ESP_ERR_INVALID_ARG;
    }
    if (endpoint == 0 || endpoint > 240) {
        set_err(err, err_size, "bad endpoint");
        return ESP_ERR_INVALID_ARG;
    }
    if (cluster_id == 0) {
        set_err(err, err_size, "bad cluster_id");
        return ESP_ERR_INVALID_ARG;
    }

    switch (cluster_id) {
        case 0x0006: {
            gw_zigbee_onoff_cmd_t ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
            if (cmd_id == 0x00) ocmd = GW_ZIGBEE_ONOFF_CMD_OFF;
            else if (cmd_id == 0x01) ocmd = GW_ZIGBEE_ONOFF_CMD_ON;
            else if (cmd_id == 0x02) ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
            else {
                set_err(err, err_size, "unsupported onoff cmd");
                return ESP_ERR_NOT_SUPPORTED;
            }
            return gw_zigbee_onoff_cmd(uid, endpoint, ocmd);
        }

        case 0x0008: {
            if (arg0 > 254) {
                set_err(err, err_size, "bad level");
                return ESP_ERR_INVALID_ARG;
            }
            if (arg1 > 60000) {
                set_err(err, err_size, "bad transition_ms");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_level_t p = {.level = (uint8_t)arg0, .transition_ms = (uint16_t)arg1};
            return gw_zigbee_level_move_to_level(uid, endpoint, p);
        }

        case 0x0300: {
            if (arg1 > 60000) {
                set_err(err, err_size, "bad transition_ms");
                return ESP_ERR_INVALID_ARG;
            }
            if (arg0 > 1000) {
                gw_zigbee_color_xy_t p = {
                    .x = (uint16_t)arg0,
                    .y = (uint16_t)arg1,
                    .transition_ms = (uint16_t)arg2
                };
                return gw_zigbee_color_move_to_xy(uid, endpoint, p);
            } else {
                gw_zigbee_color_temp_t p = {
                    .mireds = (uint16_t)arg0,
                    .transition_ms = (uint16_t)arg1
                };
                return gw_zigbee_color_move_to_temp(uid, endpoint, p);
            }
        }

        default:
            set_err(err, err_size, "unsupported cluster");
            return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t exec_group_cluster_cmd(uint16_t group_id,
                                        uint16_t cluster_id,
                                        uint8_t cmd_id,
                                        uint32_t arg0,
                                        uint32_t arg1,
                                        uint32_t arg2,
                                        char *err,
                                        size_t err_size)
{
    if (group_id == 0 || group_id == 0xFFFF) {
        set_err(err, err_size, "bad group_id");
        return ESP_ERR_INVALID_ARG;
    }
    if (cluster_id == 0) {
        set_err(err, err_size, "bad cluster_id");
        return ESP_ERR_INVALID_ARG;
    }

    switch (cluster_id) {
        case 0x0006: {
            gw_zigbee_onoff_cmd_t ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
            if (cmd_id == 0x00) ocmd = GW_ZIGBEE_ONOFF_CMD_OFF;
            else if (cmd_id == 0x01) ocmd = GW_ZIGBEE_ONOFF_CMD_ON;
            else if (cmd_id == 0x02) ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
            else {
                set_err(err, err_size, "unsupported onoff cmd");
                return ESP_ERR_NOT_SUPPORTED;
            }
            return gw_zigbee_group_onoff_cmd(group_id, ocmd);
        }

        case 0x0008: {
            if (arg0 > 254) {
                set_err(err, err_size, "bad level");
                return ESP_ERR_INVALID_ARG;
            }
            if (arg1 > 60000) {
                set_err(err, err_size, "bad transition_ms");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_level_t p = {.level = (uint8_t)arg0, .transition_ms = (uint16_t)arg1};
            return gw_zigbee_group_level_move_to_level(group_id, p);
        }

        case 0x0300: {
            if (arg1 > 60000) {
                set_err(err, err_size, "bad transition_ms");
                return ESP_ERR_INVALID_ARG;
            }
            if (arg0 > 1000) {
                gw_zigbee_color_xy_t p = {
                    .x = (uint16_t)arg0,
                    .y = (uint16_t)arg1,
                    .transition_ms = (uint16_t)arg2
                };
                return gw_zigbee_group_color_move_to_xy(group_id, p);
            } else {
                gw_zigbee_color_temp_t p = {
                    .mireds = (uint16_t)arg0,
                    .transition_ms = (uint16_t)arg1
                };
                return gw_zigbee_group_color_move_to_temp(group_id, p);
            }
        }

        default:
            set_err(err, err_size, "unsupported group cluster");
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t gw_action_exec_action(const gw_automation_entry_t *entry,
                                const gw_auto_bin_action_v2_t *action,
                                char *err,
                                size_t err_size)
{
    set_err(err, err_size, NULL);
    if (!entry || !action) {
        set_err(err, err_size, "bad args");
        return ESP_ERR_INVALID_ARG;
    }

    const char *cmd_str = strtab_at_entry(entry, action->cmd_off);
    uint8_t cmd_id = 0;
    if (cmd_str && cmd_str[0] != '\0') {
        if (cmd_str[0] == '0' && (cmd_str[1] == 'x' || cmd_str[1] == 'X')) {
            cmd_id = (uint8_t)strtol(cmd_str, NULL, 16);
        } else {
            cmd_id = (uint8_t)atoi(cmd_str);
        }
    }

    switch (action->kind) {
        case GW_AUTO_ACT_DEVICE: {
            const char *uid_s = strtab_at_entry(entry, action->uid_off);
            gw_device_uid_t uid = {0};
            if (uid_s) {
                strlcpy(uid.uid, uid_s, sizeof(uid.uid));
            }
            if (uid.uid[0] == '\0') {
                set_err(err, err_size, "missing device_uid");
                return ESP_ERR_INVALID_ARG;
            }
            return exec_device_cluster_cmd(&uid, action->endpoint, action->cluster_id, cmd_id,
                                           action->arg0_u32, action->arg1_u32, action->arg2_u32,
                                           err, err_size);
        }

        case GW_AUTO_ACT_GROUP: {
            return exec_group_cluster_cmd((uint16_t)action->group_off, action->cluster_id, cmd_id,
                                           action->arg0_u32, action->arg1_u32, action->arg2_u32,
                                           err, err_size);
        }

        case GW_AUTO_ACT_SCENE: {
            const uint16_t group_id = (uint16_t)action->arg0_u32;
            const uint8_t scene_id = (uint8_t)action->arg1_u32;
            if (group_id == 0 || group_id == 0xFFFF) {
                set_err(err, err_size, "bad group_id");
                return ESP_ERR_INVALID_ARG;
            }
            if (scene_id == 0) {
                set_err(err, err_size, "bad scene_id");
                return ESP_ERR_INVALID_ARG;
            }
            if (cmd_id == 0x00 || cmd_id == 0x40) {
                return gw_zigbee_scene_store(group_id, scene_id);
            }
            if (cmd_id == 0x01 || cmd_id == 0x41) {
                return gw_zigbee_scene_recall(group_id, scene_id);
            }
            set_err(err, err_size, "bad scene cmd");
            return ESP_ERR_INVALID_ARG;
        }

        case GW_AUTO_ACT_BIND: {
            const char *src_uid_s = strtab_at_entry(entry, action->uid_off);
            const char *dst_uid_s = strtab_at_entry(entry, action->group_off);
            gw_device_uid_t src = {0};
            gw_device_uid_t dst = {0};
            if (src_uid_s) strlcpy(src.uid, src_uid_s, sizeof(src.uid));
            if (dst_uid_s) strlcpy(dst.uid, dst_uid_s, sizeof(dst.uid));

            if (src.uid[0] == '\0' || dst.uid[0] == '\0') {
                set_err(err, err_size, "missing device uid");
                return ESP_ERR_INVALID_ARG;
            }
            if (action->endpoint == 0) {
                set_err(err, err_size, "bad endpoint");
                return ESP_ERR_INVALID_ARG;
            }
            if (action->arg0_u32 == 0) {
                set_err(err, err_size, "bad cluster_id");
                return ESP_ERR_INVALID_ARG;
            }

            const bool unbind = (action->reserved[0] & GW_AUTO_ACT_FLAG_UNBIND) != 0;
            return unbind ? gw_zigbee_unbind(&src, action->endpoint, (uint16_t)action->arg0_u32, &dst, 1)
                          : gw_zigbee_bind(&src, action->endpoint, (uint16_t)action->arg0_u32, &dst, 1);
        }

        default:
            set_err(err, err_size, "unsupported action.kind");
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t gw_action_exec_entry(const gw_automation_entry_t *entry,
                               char *err,
                               size_t err_size)
{
    set_err(err, err_size, NULL);
    if (!entry) {
        set_err(err, err_size, "bad args");
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t ai = 0; ai < entry->actions_count; ai++) {
        esp_err_t rc = gw_action_exec_action(entry, &entry->actions[ai], err, err_size);
        if (rc != ESP_OK) {
            return rc;
        }
    }
    return ESP_OK;
}
