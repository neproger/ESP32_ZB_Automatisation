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

esp_err_t gw_action_exec_compiled_zigbee(const char *cmd,
                                        const gw_device_uid_t *device_uid,
                                        uint8_t endpoint,
                                        uint32_t arg0_u32,
                                        uint32_t arg1_u32,
                                        uint32_t arg2_u32,
                                        char *err,
                                        size_t err_size)
{
    (void)arg2_u32;

    set_err(err, err_size, NULL);
    if (!cmd || cmd[0] == '\0') {
        set_err(err, err_size, "missing cmd");
        return ESP_ERR_INVALID_ARG;
    }
    if (!device_uid || device_uid->uid[0] == '\0') {
        set_err(err, err_size, "missing device_uid");
        return ESP_ERR_INVALID_ARG;
    }
    if (endpoint == 0) {
        set_err(err, err_size, "bad endpoint");
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(cmd, "onoff.", 6) == 0) {
        gw_zigbee_onoff_cmd_t ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
        if (strcmp(cmd, "onoff.off") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_OFF;
        else if (strcmp(cmd, "onoff.on") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_ON;
        else if (strcmp(cmd, "onoff.toggle") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
        else {
            set_err(err, err_size, "bad cmd");
            return ESP_ERR_INVALID_ARG;
        }
        return gw_zigbee_onoff_cmd(device_uid, endpoint, ocmd);
    }

    if (strcmp(cmd, "level.move_to_level") == 0) {
        if (arg0_u32 > 254) {
            set_err(err, err_size, "bad level");
            return ESP_ERR_INVALID_ARG;
        }
        if (arg1_u32 > 60000) {
            set_err(err, err_size, "bad transition_ms");
            return ESP_ERR_INVALID_ARG;
        }
        gw_zigbee_level_t p = {.level = (uint8_t)arg0_u32, .transition_ms = (uint16_t)arg1_u32};
        return gw_zigbee_level_move_to_level(device_uid, endpoint, p);
    }

    set_err(err, err_size, "unsupported cmd");
    return ESP_ERR_NOT_SUPPORTED;
}

static const char *strtab_at_entry(const gw_automation_entry_t *entry, uint32_t off)
{
    if (!entry) return "";
    if (off == 0) return "";
    if (off >= entry->string_table_size) return "";
    return entry->string_table + off;
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

    const char *cmd = strtab_at_entry(entry, action->cmd_off);
    if (!cmd || cmd[0] == '\0') {
        set_err(err, err_size, "missing cmd");
        return ESP_ERR_INVALID_ARG;
    }

    // Device (unicast)
    if (action->kind == GW_AUTO_ACT_DEVICE) {
        const char *uid_s = strtab_at_entry(entry, action->uid_off);
        gw_device_uid_t uid = {0};
        strlcpy(uid.uid, uid_s, sizeof(uid.uid));

        if (strcmp(cmd, "color.move_to_color_xy") == 0) {
            if (action->arg0_u32 > 65535 || action->arg1_u32 > 65535) {
                set_err(err, err_size, "bad x/y");
                return ESP_ERR_INVALID_ARG;
            }
            if (action->arg2_u32 > 60000) {
                set_err(err, err_size, "bad transition_ms");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_xy_t p = {.x = (uint16_t)action->arg0_u32, .y = (uint16_t)action->arg1_u32, .transition_ms = (uint16_t)action->arg2_u32};
            return gw_zigbee_color_move_to_xy(&uid, action->endpoint, p);
        }
        if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
            if (action->arg0_u32 < 1 || action->arg0_u32 > 1000) {
                set_err(err, err_size, "bad mireds");
                return ESP_ERR_INVALID_ARG;
            }
            if (action->arg1_u32 > 60000) {
                set_err(err, err_size, "bad transition_ms");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_temp_t p = {.mireds = (uint16_t)action->arg0_u32, .transition_ms = (uint16_t)action->arg1_u32};
            return gw_zigbee_color_move_to_temp(&uid, action->endpoint, p);
        }

        return gw_action_exec_compiled_zigbee(cmd,
                                             &uid,
                                             action->endpoint,
                                             action->arg0_u32,
                                             action->arg1_u32,
                                             action->arg2_u32,
                                             err,
                                             err_size);
    }

    // Group (groupcast)
    if (action->kind == GW_AUTO_ACT_GROUP) {
        const uint16_t group_id = action->u16_0;
        if (group_id == 0 || group_id == 0xFFFF) {
            set_err(err, err_size, "bad group_id");
            return ESP_ERR_INVALID_ARG;
        }

        if (strncmp(cmd, "onoff.", 6) == 0) {
            gw_zigbee_onoff_cmd_t ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
            if (strcmp(cmd, "onoff.off") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_OFF;
            else if (strcmp(cmd, "onoff.on") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_ON;
            else if (strcmp(cmd, "onoff.toggle") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
            else {
                set_err(err, err_size, "bad cmd");
                return ESP_ERR_INVALID_ARG;
            }
            return gw_zigbee_group_onoff_cmd(group_id, ocmd);
        }

        if (strcmp(cmd, "level.move_to_level") == 0) {
            if (action->arg0_u32 > 254) {
                set_err(err, err_size, "bad level");
                return ESP_ERR_INVALID_ARG;
            }
            if (action->arg1_u32 > 60000) {
                set_err(err, err_size, "bad transition_ms");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_level_t p = {.level = (uint8_t)action->arg0_u32, .transition_ms = (uint16_t)action->arg1_u32};
            return gw_zigbee_group_level_move_to_level(group_id, p);
        }

        if (strcmp(cmd, "color.move_to_color_xy") == 0) {
            if (action->arg0_u32 > 65535 || action->arg1_u32 > 65535) {
                set_err(err, err_size, "bad x/y");
                return ESP_ERR_INVALID_ARG;
            }
            if (action->arg2_u32 > 60000) {
                set_err(err, err_size, "bad transition_ms");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_xy_t p = {.x = (uint16_t)action->arg0_u32, .y = (uint16_t)action->arg1_u32, .transition_ms = (uint16_t)action->arg2_u32};
            return gw_zigbee_group_color_move_to_xy(group_id, p);
        }

        if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
            if (action->arg0_u32 < 1 || action->arg0_u32 > 1000) {
                set_err(err, err_size, "bad mireds");
                return ESP_ERR_INVALID_ARG;
            }
            if (action->arg1_u32 > 60000) {
                set_err(err, err_size, "bad transition_ms");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_temp_t p = {.mireds = (uint16_t)action->arg0_u32, .transition_ms = (uint16_t)action->arg1_u32};
            return gw_zigbee_group_color_move_to_temp(group_id, p);
        }

        set_err(err, err_size, "unsupported group cmd");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Scenes (group-based)
    if (action->kind == GW_AUTO_ACT_SCENE) {
        const uint16_t group_id = action->u16_0;
        const uint8_t scene_id = (uint8_t)action->u16_1;
        if (group_id == 0 || group_id == 0xFFFF) {
            set_err(err, err_size, "bad group_id");
            return ESP_ERR_INVALID_ARG;
        }
        if (scene_id == 0) {
            set_err(err, err_size, "bad scene_id");
            return ESP_ERR_INVALID_ARG;
        }

        if (strcmp(cmd, "scene.store") == 0) {
            return gw_zigbee_scene_store(group_id, scene_id);
        }
        if (strcmp(cmd, "scene.recall") == 0) {
            return gw_zigbee_scene_recall(group_id, scene_id);
        }
        set_err(err, err_size, "bad cmd");
        return ESP_ERR_INVALID_ARG;
    }

    // Binding / unbinding (ZDO)
    if (action->kind == GW_AUTO_ACT_BIND) {
        const char *src_uid_s = strtab_at_entry(entry, action->uid_off);
        const char *dst_uid_s = strtab_at_entry(entry, action->uid2_off);
        gw_device_uid_t src = {0};
        gw_device_uid_t dst = {0};
        strlcpy(src.uid, src_uid_s, sizeof(src.uid));
        strlcpy(dst.uid, dst_uid_s, sizeof(dst.uid));

        if (src.uid[0] == '\0' || dst.uid[0] == '\0') {
            set_err(err, err_size, "missing device uid");
            return ESP_ERR_INVALID_ARG;
        }
        if (action->endpoint == 0 || action->aux_ep == 0) {
            set_err(err, err_size, "bad endpoint");
            return ESP_ERR_INVALID_ARG;
        }
        if (action->u16_0 == 0) {
            set_err(err, err_size, "bad cluster_id");
            return ESP_ERR_INVALID_ARG;
        }

        const bool unbind = (action->flags & GW_AUTO_ACT_FLAG_UNBIND) != 0;
        return unbind ? gw_zigbee_unbind(&src, action->endpoint, action->u16_0, &dst, action->aux_ep)
                      : gw_zigbee_bind(&src, action->endpoint, action->u16_0, &dst, action->aux_ep);
    }

    set_err(err, err_size, "unsupported action.kind");
    return ESP_ERR_NOT_SUPPORTED;
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

