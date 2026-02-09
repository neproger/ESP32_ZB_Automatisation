#include "gw_core/action_exec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_core/cbor.h"
#include "gw_core/types.h"
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

static bool cbor_get_text0(const uint8_t *buf, size_t len, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    gw_cbor_slice_t s = {0};
    if (!gw_cbor_map_find(buf, len, key, &s)) return false;
    const uint8_t *p = NULL;
    size_t n = 0;
    if (!gw_cbor_slice_to_text_span(&s, &p, &n)) return false;
    if (n + 1 > out_size) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool cbor_has_key(const uint8_t *buf, size_t len, const char *key)
{
    gw_cbor_slice_t s = {0};
    return gw_cbor_map_find(buf, len, key, &s);
}

static bool cbor_get_u64(const uint8_t *buf, size_t len, const char *key, uint64_t *out)
{
    gw_cbor_slice_t s = {0};
    if (!gw_cbor_map_find(buf, len, key, &s)) return false;
    return gw_cbor_slice_to_u64(&s, out);
}

static bool cbor_get_i64(const uint8_t *buf, size_t len, const char *key, int64_t *out)
{
    gw_cbor_slice_t s = {0};
    if (!gw_cbor_map_find(buf, len, key, &s)) return false;
    return gw_cbor_slice_to_i64(&s, out);
}

static bool cbor_get_u16(const uint8_t *buf, size_t len, const char *key, uint16_t *out)
{
    uint64_t v = 0;
    if (!cbor_get_u64(buf, len, key, &v)) {
        int64_t iv = 0;
        if (!cbor_get_i64(buf, len, key, &iv)) return false;
        if (iv < 0 || iv > 65535) return false;
        v = (uint64_t)iv;
    }
    if (v > 65535) return false;
    *out = (uint16_t)v;
    return true;
}

static bool cbor_get_u8(const uint8_t *buf, size_t len, const char *key, uint8_t *out, uint8_t min_v, uint8_t max_v)
{
    uint64_t v = 0;
    if (!cbor_get_u64(buf, len, key, &v)) {
        int64_t iv = 0;
        if (!cbor_get_i64(buf, len, key, &iv)) return false;
        if (iv < 0) return false;
        v = (uint64_t)iv;
    }
    if (v < min_v || v > max_v) return false;
    *out = (uint8_t)v;
    return true;
}

static bool cbor_get_u16_ms_opt(const uint8_t *buf, size_t len, const char *key, uint16_t *out, uint16_t max_v)
{
    if (!cbor_has_key(buf, len, key)) {
        *out = 0;
        return true;
    }
    uint16_t v = 0;
    if (!cbor_get_u16(buf, len, key, &v)) return false;
    if (v > max_v) return false;
    *out = v;
    return true;
}

static bool cbor_get_uid(const uint8_t *buf, size_t len, const char *key, gw_device_uid_t *out)
{
    if (!out) return false;
    char tmp[GW_DEVICE_UID_STRLEN] = {0};
    if (!cbor_get_text0(buf, len, key, tmp, sizeof(tmp))) return false;
    if (tmp[0] == '\0') return false;
    memset(out, 0, sizeof(*out));
    strlcpy(out->uid, tmp, sizeof(out->uid));
    return true;
}

esp_err_t gw_action_exec_cbor(const uint8_t *buf, size_t len, char *err, size_t err_size)
{
    set_err(err, err_size, NULL);
    if (!buf || len == 0) {
        set_err(err, err_size, "bad action");
        return ESP_ERR_INVALID_ARG;
    }

    char type[16] = {0};
    if (!cbor_get_text0(buf, len, "type", type, sizeof(type))) {
        set_err(err, err_size, "missing type");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(type, "zigbee") != 0) {
        set_err(err, err_size, "unsupported type");
        return ESP_ERR_NOT_SUPPORTED;
    }

    char cmd[64] = {0};
    if (!cbor_get_text0(buf, len, "cmd", cmd, sizeof(cmd))) {
        set_err(err, err_size, "missing cmd");
        return ESP_ERR_INVALID_ARG;
    }

    const bool has_group = cbor_has_key(buf, len, "group_id");
    const bool has_uid = cbor_has_key(buf, len, "device_uid") || cbor_has_key(buf, len, "uid");

    // Scenes
    if (strcmp(cmd, "scene.store") == 0 || strcmp(cmd, "scene.recall") == 0) {
        uint16_t gid = 0;
        uint8_t sid = 0;
        if (!cbor_get_u16(buf, len, "group_id", &gid) || gid == 0 || gid == 0xFFFF) {
            set_err(err, err_size, "bad group_id");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_get_u8(buf, len, "scene_id", &sid, 1, 255)) {
            set_err(err, err_size, "bad scene_id");
            return ESP_ERR_INVALID_ARG;
        }
        return (strcmp(cmd, "scene.store") == 0) ? gw_zigbee_scene_store(gid, sid) : gw_zigbee_scene_recall(gid, sid);
    }

    // Bind/unbind
    if (strcmp(cmd, "bind") == 0 || strcmp(cmd, "unbind") == 0) {
        gw_device_uid_t src_uid = {0};
        gw_device_uid_t dst_uid = {0};
        if (!cbor_get_uid(buf, len, "src_device_uid", &src_uid) && !cbor_get_uid(buf, len, "src_uid", &src_uid)) {
            set_err(err, err_size, "missing src_device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_get_uid(buf, len, "dst_device_uid", &dst_uid) && !cbor_get_uid(buf, len, "dst_uid", &dst_uid)) {
            set_err(err, err_size, "missing dst_device_uid");
            return ESP_ERR_INVALID_ARG;
        }

        uint8_t src_ep = 0;
        uint8_t dst_ep = 0;
        uint16_t cluster_id = 0;
        if (!cbor_get_u8(buf, len, "src_endpoint", &src_ep, 1, 240)) {
            set_err(err, err_size, "bad src_endpoint");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_get_u8(buf, len, "dst_endpoint", &dst_ep, 1, 240)) {
            set_err(err, err_size, "bad dst_endpoint");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_get_u16(buf, len, "cluster_id", &cluster_id) || cluster_id == 0) {
            set_err(err, err_size, "bad cluster_id");
            return ESP_ERR_INVALID_ARG;
        }

        return (strcmp(cmd, "bind") == 0) ? gw_zigbee_bind(&src_uid, src_ep, cluster_id, &dst_uid, dst_ep)
                                           : gw_zigbee_unbind(&src_uid, src_ep, cluster_id, &dst_uid, dst_ep);
    }

    // On/off
    if (strncmp(cmd, "onoff.", 6) == 0 || (strcmp(cmd, "on") == 0 || strcmp(cmd, "off") == 0 || strcmp(cmd, "toggle") == 0)) {
        const char *use_cmd = cmd;
        if (strcmp(cmd, "on") == 0) use_cmd = "onoff.on";
        else if (strcmp(cmd, "off") == 0) use_cmd = "onoff.off";
        else if (strcmp(cmd, "toggle") == 0) use_cmd = "onoff.toggle";

        gw_zigbee_onoff_cmd_t ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
        if (strcmp(use_cmd, "onoff.off") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_OFF;
        else if (strcmp(use_cmd, "onoff.on") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_ON;
        else if (strcmp(use_cmd, "onoff.toggle") == 0) ocmd = GW_ZIGBEE_ONOFF_CMD_TOGGLE;
        else {
            set_err(err, err_size, "bad cmd");
            return ESP_ERR_INVALID_ARG;
        }

        if (has_group) {
            uint16_t gid = 0;
            if (!cbor_get_u16(buf, len, "group_id", &gid) || gid == 0 || gid == 0xFFFF) {
                set_err(err, err_size, "bad group_id");
                return ESP_ERR_INVALID_ARG;
            }
            return gw_zigbee_group_onoff_cmd(gid, ocmd);
        }

        if (!has_uid) {
            set_err(err, err_size, "missing device_uid");
            return ESP_ERR_INVALID_ARG;
        }

        gw_device_uid_t uid = {0};
        if (!cbor_get_uid(buf, len, "device_uid", &uid) && !cbor_get_uid(buf, len, "uid", &uid)) {
            set_err(err, err_size, "missing device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t endpoint = 0;
        if (!cbor_get_u8(buf, len, "endpoint", &endpoint, 1, 240)) {
            set_err(err, err_size, "bad endpoint");
            return ESP_ERR_INVALID_ARG;
        }
        return gw_zigbee_onoff_cmd(&uid, endpoint, ocmd);
    }

    // Level
    if (strncmp(cmd, "level.", 6) == 0) {
        if (strcmp(cmd, "level.move_to_level") != 0) {
            set_err(err, err_size, "bad cmd");
            return ESP_ERR_INVALID_ARG;
        }

        uint8_t level = 0;
        if (!cbor_get_u8(buf, len, "level", &level, 0, 254)) {
            set_err(err, err_size, "bad level");
            return ESP_ERR_INVALID_ARG;
        }
        uint16_t transition_ms = 0;
        if (!cbor_get_u16_ms_opt(buf, len, "transition_ms", &transition_ms, 60000)) {
            set_err(err, err_size, "bad transition_ms");
            return ESP_ERR_INVALID_ARG;
        }

        if (has_group) {
            uint16_t gid = 0;
            if (!cbor_get_u16(buf, len, "group_id", &gid) || gid == 0 || gid == 0xFFFF) {
                set_err(err, err_size, "bad group_id");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_level_t p = {.level = level, .transition_ms = transition_ms};
            return gw_zigbee_group_level_move_to_level(gid, p);
        }

        gw_device_uid_t uid = {0};
        if (!cbor_get_uid(buf, len, "device_uid", &uid) && !cbor_get_uid(buf, len, "uid", &uid)) {
            set_err(err, err_size, "missing device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t endpoint = 0;
        if (!cbor_get_u8(buf, len, "endpoint", &endpoint, 1, 240)) {
            set_err(err, err_size, "bad endpoint");
            return ESP_ERR_INVALID_ARG;
        }
        gw_zigbee_level_t p = {.level = level, .transition_ms = transition_ms};
        return gw_zigbee_level_move_to_level(&uid, endpoint, p);
    }

    // Color
    if (strncmp(cmd, "color.", 6) == 0) {
        uint16_t transition_ms = 0;
        if (!cbor_get_u16_ms_opt(buf, len, "transition_ms", &transition_ms, 60000)) {
            set_err(err, err_size, "bad transition_ms");
            return ESP_ERR_INVALID_ARG;
        }

        if (has_group) {
            uint16_t gid = 0;
            if (!cbor_get_u16(buf, len, "group_id", &gid) || gid == 0 || gid == 0xFFFF) {
                set_err(err, err_size, "bad group_id");
                return ESP_ERR_INVALID_ARG;
            }
            if (strcmp(cmd, "color.move_to_color_xy") == 0) {
                uint16_t x = 0, y = 0;
                if (!cbor_get_u16(buf, len, "x", &x)) {
                    set_err(err, err_size, "bad x");
                    return ESP_ERR_INVALID_ARG;
                }
                if (!cbor_get_u16(buf, len, "y", &y)) {
                    set_err(err, err_size, "bad y");
                    return ESP_ERR_INVALID_ARG;
                }
                gw_zigbee_color_xy_t p = {.x = x, .y = y, .transition_ms = transition_ms};
                return gw_zigbee_group_color_move_to_xy(gid, p);
            }
            if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
                uint16_t mireds = 0;
                if (!cbor_get_u16(buf, len, "mireds", &mireds) || mireds < 1 || mireds > 1000) {
                    set_err(err, err_size, "bad mireds");
                    return ESP_ERR_INVALID_ARG;
                }
                gw_zigbee_color_temp_t p = {.mireds = mireds, .transition_ms = transition_ms};
                return gw_zigbee_group_color_move_to_temp(gid, p);
            }
            set_err(err, err_size, "bad cmd");
            return ESP_ERR_INVALID_ARG;
        }

        gw_device_uid_t uid = {0};
        if (!cbor_get_uid(buf, len, "device_uid", &uid) && !cbor_get_uid(buf, len, "uid", &uid)) {
            set_err(err, err_size, "missing device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t endpoint = 0;
        if (!cbor_get_u8(buf, len, "endpoint", &endpoint, 1, 240)) {
            set_err(err, err_size, "bad endpoint");
            return ESP_ERR_INVALID_ARG;
        }

        if (strcmp(cmd, "color.move_to_color_xy") == 0) {
            uint16_t x = 0, y = 0;
            if (!cbor_get_u16(buf, len, "x", &x)) {
                set_err(err, err_size, "bad x");
                return ESP_ERR_INVALID_ARG;
            }
            if (!cbor_get_u16(buf, len, "y", &y)) {
                set_err(err, err_size, "bad y");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_xy_t p = {.x = x, .y = y, .transition_ms = transition_ms};
            return gw_zigbee_color_move_to_xy(&uid, endpoint, p);
        }
        if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
            uint16_t mireds = 0;
            if (!cbor_get_u16(buf, len, "mireds", &mireds) || mireds < 1 || mireds > 1000) {
                set_err(err, err_size, "bad mireds");
                return ESP_ERR_INVALID_ARG;
            }
            gw_zigbee_color_temp_t p = {.mireds = mireds, .transition_ms = transition_ms};
            return gw_zigbee_color_move_to_temp(&uid, endpoint, p);
        }

        set_err(err, err_size, "bad cmd");
        return ESP_ERR_INVALID_ARG;
    }

    set_err(err, err_size, "unknown cmd");
    return ESP_ERR_NOT_SUPPORTED;
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

static const char *strtab_at(const gw_auto_compiled_t *c, uint32_t off)
{
    if (!c || !c->strings) return "";
    if (off == 0) return "";
    if (off >= c->hdr.strings_size) return "";
    return c->strings + off;
}

esp_err_t gw_action_exec_compiled(const gw_auto_compiled_t *compiled,
                                 const gw_auto_bin_action_v2_t *action,
                                 char *err,
                                 size_t err_size)
{
    set_err(err, err_size, NULL);
    if (!compiled || !action) {
        set_err(err, err_size, "bad args");
        return ESP_ERR_INVALID_ARG;
    }

    const char *cmd = strtab_at(compiled, action->cmd_off);
    if (!cmd || cmd[0] == '\0') {
        set_err(err, err_size, "missing cmd");
        return ESP_ERR_INVALID_ARG;
    }

    // Device (unicast)
    if (action->kind == GW_AUTO_ACT_DEVICE) {
        const char *uid_s = strtab_at(compiled, action->uid_off);
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
        const char *src_uid_s = strtab_at(compiled, action->uid_off);
        const char *dst_uid_s = strtab_at(compiled, action->uid2_off);
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

