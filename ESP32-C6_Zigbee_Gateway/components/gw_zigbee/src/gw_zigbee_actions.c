#include "gw_zigbee/gw_zigbee.h"

#include <stdlib.h>

#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_on_off.h"

#include "gw_core/device_registry.h"
#include "gw_zigbee_internal.h"

typedef struct {
    uint8_t endpoint;
    uint16_t short_addr;
    uint8_t address_mode;
    gw_device_uid_t uid;
    enum {
        GW_ZB_ACTION_ONOFF = 1,
        GW_ZB_ACTION_LEVEL_MOVE_TO_LEVEL = 2,
        GW_ZB_ACTION_COLOR_MOVE_TO_XY = 3,
        GW_ZB_ACTION_COLOR_MOVE_TO_TEMP = 4,
        GW_ZB_ACTION_SCENE_STORE = 5,
        GW_ZB_ACTION_SCENE_RECALL = 6,
    } type;
    union {
        struct {
            gw_zigbee_onoff_cmd_t cmd;
        } onoff;
        struct {
            uint8_t level;
            uint16_t transition_ds;
        } level;
        struct {
            uint16_t x;
            uint16_t y;
            uint16_t transition_ds;
        } color_xy;
        struct {
            uint16_t mireds;
            uint16_t transition_ds;
        } color_temp;
        struct {
            uint16_t group_id;
            uint8_t scene_id;
        } scene;
    } u;
} gw_zb_action_ctx_t;

static gw_zb_action_ctx_t *s_action_ctx_by_token[256];
static uint8_t s_action_token;
static portMUX_TYPE s_action_lock = portMUX_INITIALIZER_UNLOCKED;

static uint16_t transition_ms_to_ds(uint16_t ms)
{
    uint32_t ds = (uint32_t)(ms + 50u) / 100u;
    if (ds > 0xFFFFu) {
        ds = 0xFFFFu;
    }
    return (uint16_t)ds;
}

static void action_send_cb(uint8_t token)
{
    gw_zb_action_ctx_t *ctx = NULL;
    portENTER_CRITICAL(&s_action_lock);
    ctx = s_action_ctx_by_token[token];
    s_action_ctx_by_token[token] = NULL;
    portEXIT_CRITICAL(&s_action_lock);

    if (ctx == NULL) {
        return;
    }

    uint8_t tsn = 0;

    if (ctx->type == GW_ZB_ACTION_ONOFF) {
        uint8_t cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID;
        switch (ctx->u.onoff.cmd) {
        case GW_ZIGBEE_ONOFF_CMD_OFF: cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID; break;
        case GW_ZIGBEE_ONOFF_CMD_ON: cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_ON_ID; break;
        case GW_ZIGBEE_ONOFF_CMD_TOGGLE:
        default: cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID; break;
        }

        esp_zb_zcl_on_off_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = ctx->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ctx->address_mode;
        cmd.on_off_cmd_id = cmd_id;
        tsn = esp_zb_zcl_on_off_cmd_req(&cmd);

        gw_zigbee_log_device_action("sent", ctx->uid.uid, ctx->short_addr, ctx->endpoint,
            (ctx->u.onoff.cmd == GW_ZIGBEE_ONOFF_CMD_OFF) ? "off" : (ctx->u.onoff.cmd == GW_ZIGBEE_ONOFF_CMD_ON) ? "on" : "toggle",
            "0x0006", token, tsn);
    } else if (ctx->type == GW_ZB_ACTION_LEVEL_MOVE_TO_LEVEL) {
        esp_zb_zcl_move_to_level_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = ctx->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ctx->address_mode;
        cmd.level = ctx->u.level.level;
        cmd.transition_time = ctx->u.level.transition_ds;
        tsn = esp_zb_zcl_level_move_to_level_cmd_req(&cmd);
        gw_zigbee_log_device_action("sent", ctx->uid.uid, ctx->short_addr, ctx->endpoint, "move_to_level", "0x0008", ctx->u.level.level, ctx->u.level.transition_ds);
    } else if (ctx->type == GW_ZB_ACTION_COLOR_MOVE_TO_XY) {
        esp_zb_zcl_color_move_to_color_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = ctx->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ctx->address_mode;
        cmd.color_x = ctx->u.color_xy.x;
        cmd.color_y = ctx->u.color_xy.y;
        cmd.transition_time = ctx->u.color_xy.transition_ds;
        tsn = esp_zb_zcl_color_move_to_color_cmd_req(&cmd);
        gw_zigbee_log_device_action("sent", ctx->uid.uid, ctx->short_addr, ctx->endpoint, "move_to_color_xy", "0x0300", ctx->u.color_xy.x, ctx->u.color_xy.y);
    } else if (ctx->type == GW_ZB_ACTION_COLOR_MOVE_TO_TEMP) {
        esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = ctx->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ctx->address_mode;
        cmd.color_temperature = ctx->u.color_temp.mireds;
        cmd.transition_time = ctx->u.color_temp.transition_ds;
        tsn = esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd);
        gw_zigbee_log_device_action("sent", ctx->uid.uid, ctx->short_addr, ctx->endpoint, "move_to_color_temperature", "0x0300", ctx->u.color_temp.mireds, ctx->u.color_temp.transition_ds);
    } else if (ctx->type == GW_ZB_ACTION_SCENE_STORE) {
        esp_zb_zcl_scenes_store_scene_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = ctx->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ctx->address_mode;
        cmd.group_id = ctx->u.scene.group_id;
        cmd.scene_id = ctx->u.scene.scene_id;
        tsn = esp_zb_zcl_scenes_store_scene_cmd_req(&cmd);
        gw_zigbee_log_group_action("sent", ctx->u.scene.group_id, "scene.store", "0x0005", ctx->u.scene.scene_id, tsn);
    } else if (ctx->type == GW_ZB_ACTION_SCENE_RECALL) {
        esp_zb_zcl_scenes_recall_scene_cmd_t cmd = {0};
        cmd.zcl_basic_cmd.dst_addr_u.addr_short = ctx->short_addr;
        cmd.zcl_basic_cmd.dst_endpoint = ctx->endpoint;
        cmd.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
        cmd.address_mode = ctx->address_mode;
        cmd.group_id = ctx->u.scene.group_id;
        cmd.scene_id = ctx->u.scene.scene_id;
        tsn = esp_zb_zcl_scenes_recall_scene_cmd_req(&cmd);
        gw_zigbee_log_group_action("sent", ctx->u.scene.group_id, "scene.recall", "0x0005", ctx->u.scene.scene_id, tsn);
    }

    free(ctx);
}

static esp_err_t schedule_group_action(uint16_t group_id, gw_zb_action_ctx_t *ctx)
{
    if (group_id == 0 || group_id == 0xFFFF || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ctx->short_addr = group_id;
    ctx->endpoint = 0xFF;
    ctx->address_mode = ESP_ZB_APS_ADDR_MODE_16_GROUP_ENDP_NOT_PRESENT;
    ctx->uid.uid[0] = '\0';

    uint8_t token = 0;
    portENTER_CRITICAL(&s_action_lock);
    s_action_token++;
    if (s_action_token == 0) s_action_token++;
    token = s_action_token;
    if (s_action_ctx_by_token[token] != NULL) {
        portEXIT_CRITICAL(&s_action_lock);
        return ESP_FAIL;
    }
    s_action_ctx_by_token[token] = ctx;
    portEXIT_CRITICAL(&s_action_lock);

    gw_zigbee_lock();
    esp_zb_scheduler_alarm(action_send_cb, token, 0);
    gw_zigbee_unlock();
    return ESP_OK;
}

esp_err_t gw_zigbee_read_onoff_state(const gw_device_uid_t *uid, uint8_t endpoint)
{
    return gw_zigbee_read_attr(uid, endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
}

esp_err_t gw_zigbee_read_attr(const gw_device_uid_t *uid, uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id)
{
    if (uid == NULL || uid->uid[0] == '\0' || endpoint == 0 || cluster_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_device_t d = {0};
    esp_err_t err = gw_device_registry_get(uid, &d);
    if (err != ESP_OK) return err;
    if (d.short_addr == 0 || d.short_addr == 0xFFFF) return ESP_ERR_INVALID_STATE;

    uint16_t attrs[] = {attr_id};
    esp_zb_zcl_read_attr_cmd_t r = {0};
    r.zcl_basic_cmd.dst_addr_u.addr_short = d.short_addr;
    r.zcl_basic_cmd.dst_endpoint = endpoint;
    r.zcl_basic_cmd.src_endpoint = GW_ZIGBEE_GATEWAY_ENDPOINT;
    r.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    r.clusterID = cluster_id;
    r.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    r.attr_number = 1;
    r.attr_field = attrs;
    gw_zigbee_lock();
    (void)esp_zb_zcl_read_attr_cmd_req(&r);
    gw_zigbee_unlock();
    return ESP_OK;
}

esp_err_t gw_zigbee_onoff_cmd(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_onoff_cmd_t cmd)
{
    if (uid == NULL || uid->uid[0] == '\0' || endpoint == 0) return ESP_ERR_INVALID_ARG;
    gw_device_t d = {0};
    esp_err_t err = gw_device_registry_get(uid, &d);
    if (err != ESP_OK) return err;
    if (d.short_addr == 0 || d.short_addr == 0xFFFF) return ESP_ERR_INVALID_STATE;

    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    ctx->endpoint = endpoint;
    ctx->short_addr = d.short_addr;
    ctx->address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    ctx->uid = *uid;
    ctx->type = GW_ZB_ACTION_ONOFF;
    ctx->u.onoff.cmd = cmd;

    uint8_t token = 0;
    portENTER_CRITICAL(&s_action_lock);
    s_action_token++;
    if (s_action_token == 0) s_action_token++;
    token = s_action_token;
    if (s_action_ctx_by_token[token] != NULL) {
        portEXIT_CRITICAL(&s_action_lock);
        free(ctx);
        return ESP_FAIL;
    }
    s_action_ctx_by_token[token] = ctx;
    portEXIT_CRITICAL(&s_action_lock);

    gw_zigbee_log_device_action("queue", uid->uid, d.short_addr, endpoint,
        (cmd == GW_ZIGBEE_ONOFF_CMD_OFF) ? "off" : (cmd == GW_ZIGBEE_ONOFF_CMD_ON) ? "on" : "toggle",
        "0x0006", token, 0);
    gw_zigbee_lock();
    esp_zb_scheduler_alarm(action_send_cb, token, 0);
    gw_zigbee_unlock();
    return ESP_OK;
}

esp_err_t gw_zigbee_level_move_to_level(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_level_t level)
{
    if (uid == NULL || uid->uid[0] == '\0' || endpoint == 0 || level.level > 254) return ESP_ERR_INVALID_ARG;
    gw_device_t d = {0};
    esp_err_t err = gw_device_registry_get(uid, &d);
    if (err != ESP_OK) return err;
    if (d.short_addr == 0 || d.short_addr == 0xFFFF) return ESP_ERR_INVALID_STATE;
    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    ctx->endpoint = endpoint; ctx->short_addr = d.short_addr; ctx->address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; ctx->uid = *uid;
    ctx->type = GW_ZB_ACTION_LEVEL_MOVE_TO_LEVEL; ctx->u.level.level = level.level; ctx->u.level.transition_ds = transition_ms_to_ds(level.transition_ms);
    uint8_t token = 0; portENTER_CRITICAL(&s_action_lock); s_action_token++; if (s_action_token == 0) s_action_token++; token = s_action_token; if (s_action_ctx_by_token[token] != NULL) { portEXIT_CRITICAL(&s_action_lock); free(ctx); return ESP_FAIL; } s_action_ctx_by_token[token] = ctx; portEXIT_CRITICAL(&s_action_lock);
    gw_zigbee_log_device_action("queue", uid->uid, d.short_addr, endpoint, "move_to_level", "0x0008", ctx->u.level.level, ctx->u.level.transition_ds);
    gw_zigbee_lock(); esp_zb_scheduler_alarm(action_send_cb, token, 0); gw_zigbee_unlock(); return ESP_OK;
}

esp_err_t gw_zigbee_color_move_to_xy(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_color_xy_t color)
{
    if (uid == NULL || uid->uid[0] == '\0' || endpoint == 0) return ESP_ERR_INVALID_ARG;
    gw_device_t d = {0}; esp_err_t err = gw_device_registry_get(uid, &d); if (err != ESP_OK) return err; if (d.short_addr == 0 || d.short_addr == 0xFFFF) return ESP_ERR_INVALID_STATE;
    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx)); if (!ctx) return ESP_ERR_NO_MEM;
    ctx->endpoint = endpoint; ctx->short_addr = d.short_addr; ctx->address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; ctx->uid = *uid; ctx->type = GW_ZB_ACTION_COLOR_MOVE_TO_XY; ctx->u.color_xy.x = color.x; ctx->u.color_xy.y = color.y; ctx->u.color_xy.transition_ds = transition_ms_to_ds(color.transition_ms);
    uint8_t token = 0; portENTER_CRITICAL(&s_action_lock); s_action_token++; if (s_action_token == 0) s_action_token++; token = s_action_token; if (s_action_ctx_by_token[token] != NULL) { portEXIT_CRITICAL(&s_action_lock); free(ctx); return ESP_FAIL; } s_action_ctx_by_token[token] = ctx; portEXIT_CRITICAL(&s_action_lock);
    gw_zigbee_log_device_action("queue", uid->uid, d.short_addr, endpoint, "move_to_color_xy", "0x0300", ctx->u.color_xy.x, ctx->u.color_xy.y);
    gw_zigbee_lock(); esp_zb_scheduler_alarm(action_send_cb, token, 0); gw_zigbee_unlock(); return ESP_OK;
}

esp_err_t gw_zigbee_color_move_to_temp(const gw_device_uid_t *uid, uint8_t endpoint, gw_zigbee_color_temp_t temp)
{
    if (uid == NULL || uid->uid[0] == '\0' || endpoint == 0) return ESP_ERR_INVALID_ARG;
    gw_device_t d = {0}; esp_err_t err = gw_device_registry_get(uid, &d); if (err != ESP_OK) return err; if (d.short_addr == 0 || d.short_addr == 0xFFFF) return ESP_ERR_INVALID_STATE;
    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx)); if (!ctx) return ESP_ERR_NO_MEM;
    ctx->endpoint = endpoint; ctx->short_addr = d.short_addr; ctx->address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT; ctx->uid = *uid; ctx->type = GW_ZB_ACTION_COLOR_MOVE_TO_TEMP; ctx->u.color_temp.mireds = temp.mireds; ctx->u.color_temp.transition_ds = transition_ms_to_ds(temp.transition_ms);
    uint8_t token = 0; portENTER_CRITICAL(&s_action_lock); s_action_token++; if (s_action_token == 0) s_action_token++; token = s_action_token; if (s_action_ctx_by_token[token] != NULL) { portEXIT_CRITICAL(&s_action_lock); free(ctx); return ESP_FAIL; } s_action_ctx_by_token[token] = ctx; portEXIT_CRITICAL(&s_action_lock);
    gw_zigbee_log_device_action("queue", uid->uid, d.short_addr, endpoint, "move_to_color_temperature", "0x0300", ctx->u.color_temp.mireds, ctx->u.color_temp.transition_ds);
    gw_zigbee_lock(); esp_zb_scheduler_alarm(action_send_cb, token, 0); gw_zigbee_unlock(); return ESP_OK;
}

esp_err_t gw_zigbee_group_onoff_cmd(uint16_t group_id, gw_zigbee_onoff_cmd_t cmd)
{
    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx)); if (!ctx) return ESP_ERR_NO_MEM;
    ctx->type = GW_ZB_ACTION_ONOFF; ctx->u.onoff.cmd = cmd;
    gw_zigbee_log_group_action("queue", group_id, (cmd == GW_ZIGBEE_ONOFF_CMD_OFF) ? "off" : (cmd == GW_ZIGBEE_ONOFF_CMD_ON) ? "on" : "toggle", "0x0006", 0, 0);
    esp_err_t err = schedule_group_action(group_id, ctx); if (err != ESP_OK) free(ctx); return err;
}

esp_err_t gw_zigbee_group_level_move_to_level(uint16_t group_id, gw_zigbee_level_t level)
{
    if (level.level > 254) return ESP_ERR_INVALID_ARG;
    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx)); if (!ctx) return ESP_ERR_NO_MEM;
    ctx->type = GW_ZB_ACTION_LEVEL_MOVE_TO_LEVEL; ctx->u.level.level = level.level; ctx->u.level.transition_ds = transition_ms_to_ds(level.transition_ms);
    gw_zigbee_log_group_action("queue", group_id, "move_to_level", "0x0008", ctx->u.level.level, ctx->u.level.transition_ds);
    esp_err_t err = schedule_group_action(group_id, ctx); if (err != ESP_OK) free(ctx); return err;
}

esp_err_t gw_zigbee_group_color_move_to_xy(uint16_t group_id, gw_zigbee_color_xy_t color)
{
    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx)); if (!ctx) return ESP_ERR_NO_MEM;
    ctx->type = GW_ZB_ACTION_COLOR_MOVE_TO_XY; ctx->u.color_xy.x = color.x; ctx->u.color_xy.y = color.y; ctx->u.color_xy.transition_ds = transition_ms_to_ds(color.transition_ms);
    gw_zigbee_log_group_action("queue", group_id, "move_to_color_xy", "0x0300", ctx->u.color_xy.x, ctx->u.color_xy.y);
    esp_err_t err = schedule_group_action(group_id, ctx); if (err != ESP_OK) free(ctx); return err;
}

esp_err_t gw_zigbee_group_color_move_to_temp(uint16_t group_id, gw_zigbee_color_temp_t temp)
{
    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx)); if (!ctx) return ESP_ERR_NO_MEM;
    ctx->type = GW_ZB_ACTION_COLOR_MOVE_TO_TEMP; ctx->u.color_temp.mireds = temp.mireds; ctx->u.color_temp.transition_ds = transition_ms_to_ds(temp.transition_ms);
    gw_zigbee_log_group_action("queue", group_id, "move_to_color_temperature", "0x0300", ctx->u.color_temp.mireds, ctx->u.color_temp.transition_ds);
    esp_err_t err = schedule_group_action(group_id, ctx); if (err != ESP_OK) free(ctx); return err;
}

esp_err_t gw_zigbee_scene_store(uint16_t group_id, uint8_t scene_id)
{
    if (group_id == 0 || group_id == 0xFFFF || scene_id == 0) return ESP_ERR_INVALID_ARG;
    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx)); if (!ctx) return ESP_ERR_NO_MEM;
    ctx->type = GW_ZB_ACTION_SCENE_STORE; ctx->u.scene.group_id = group_id; ctx->u.scene.scene_id = scene_id;
    gw_zigbee_log_group_action("queue", group_id, "scene.store", "0x0005", scene_id, 0);
    esp_err_t err = schedule_group_action(group_id, ctx); if (err != ESP_OK) free(ctx); return err;
}

esp_err_t gw_zigbee_scene_recall(uint16_t group_id, uint8_t scene_id)
{
    if (group_id == 0 || group_id == 0xFFFF || scene_id == 0) return ESP_ERR_INVALID_ARG;
    gw_zb_action_ctx_t *ctx = (gw_zb_action_ctx_t *)calloc(1, sizeof(*ctx)); if (!ctx) return ESP_ERR_NO_MEM;
    ctx->type = GW_ZB_ACTION_SCENE_RECALL; ctx->u.scene.group_id = group_id; ctx->u.scene.scene_id = scene_id;
    gw_zigbee_log_group_action("queue", group_id, "scene.recall", "0x0005", scene_id, 0);
    esp_err_t err = schedule_group_action(group_id, ctx); if (err != ESP_OK) free(ctx); return err;
}
