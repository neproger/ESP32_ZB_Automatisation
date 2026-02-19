#include "gw_http/gw_rest.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gw_core/action_exec.h"
#include "gw_core/automation_store.h"
#include "gw_core/cbor.h"
#include "gw_core/device_fb_store.h"
#include "gw_core/device_registry.h"
#include "gw_core/event_bus.h"
#include "gw_core/group_store.h"
#include "gw_core/state_store.h"
#include "gw_zigbee/gw_zigbee.h"


#define GW_HTTP_MAX_BODY (16 * 1024)
#define GW_HTTP_ID_BUFFER 128

static bool gw_http_percent_decode(const char *src, char *dst, size_t dst_size);
static bool gw_http_extract_id(const char *uri, const char *prefix, char *out, size_t out_size);
static esp_err_t gw_http_recv_body(httpd_req_t *req, uint8_t **out_buf, size_t *out_len);
static esp_err_t gw_http_send_cbor_payload(httpd_req_t *req, const uint8_t *buf, size_t len);
static esp_err_t gw_action_exec_from_cbor(const uint8_t *buf, size_t len, char *err, size_t err_size);
static const char *automation_string_at(const gw_automation_entry_t *entry, uint32_t off);
static const char *automation_evt_type_to_str(uint8_t type);
static const char *automation_op_to_str(uint8_t op);
static esp_err_t cbor_write_automation_definition(gw_cbor_writer_t *w, const gw_automation_entry_t *entry);
static esp_err_t api_devices_flatbuffer_get_handler(httpd_req_t *req);
static esp_err_t api_devices_post_handler(httpd_req_t *req);
static esp_err_t api_devices_remove_post_handler(httpd_req_t *req);
static esp_err_t api_network_permit_join_post_handler(httpd_req_t *req);
static esp_err_t api_automations_get_handler(httpd_req_t *req);
static esp_err_t api_automation_detail_patch_handler(httpd_req_t *req);
static esp_err_t api_automation_detail_delete_handler(httpd_req_t *req);
static esp_err_t api_automation_post_handler(httpd_req_t *req);
static esp_err_t api_actions_post_handler(httpd_req_t *req);
static esp_err_t api_state_get_handler(httpd_req_t *req);
static esp_err_t api_groups_get_handler(httpd_req_t *req);
static esp_err_t api_groups_post_handler(httpd_req_t *req);
static esp_err_t api_group_items_get_handler(httpd_req_t *req);
static esp_err_t api_group_items_post_handler(httpd_req_t *req);
static esp_err_t gw_http_send_group_store_error(httpd_req_t *req, esp_err_t err, const char *not_found_msg, const char *no_mem_msg);
static esp_err_t gw_http_send_cbor_ok(httpd_req_t *req);

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool gw_http_percent_decode(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) {
        return false;
    }

    size_t di = 0;
    while (*src && di + 1 < dst_size) {
        if (*src == '%') {
            int hi = src[1] ? hex_digit((unsigned char)src[1]) : -1;
            int lo = src[2] ? hex_digit((unsigned char)src[2]) : -1;
            if (hi < 0 || lo < 0) {
                return false;
            }
            dst[di++] = (char)((hi << 4) | lo);
            src += 3;
            continue;
        }
        if (*src == '+') {
            dst[di++] = ' ';
            src++;
            continue;
        }
        dst[di++] = *src++;
    }
    dst[di] = '\0';
    return *src == '\0';
}

static bool gw_http_extract_id(const char *uri, const char *prefix, char *out, size_t out_size)
{
    if (!uri || !prefix || !out || out_size == 0) {
        return false;
    }
    size_t prefix_len = strlen(prefix);
    if (strncmp(uri, prefix, prefix_len) != 0) {
        return false;
    }
    const char *segment = uri + prefix_len;
    if (*segment != '/') {
        return false;
    }
    segment++;
    if (*segment == '\0') {
        return false;
    }
    const char *end = strchr(segment, '?');
    size_t raw_len = end ? (size_t)(end - segment) : strlen(segment);
    if (raw_len == 0 || raw_len >= GW_HTTP_ID_BUFFER) {
        return false;
    }
    char tmp[GW_HTTP_ID_BUFFER];
    memcpy(tmp, segment, raw_len);
    tmp[raw_len] = '\0';
    if (!gw_http_percent_decode(tmp, out, out_size)) {
        return false;
    }
    return out[0] != '\0';
}

static esp_err_t gw_http_recv_body(httpd_req_t *req, uint8_t **out_buf, size_t *out_len)
{
    if (!req || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    size_t len = req->content_len;
    if (len == 0 || len > GW_HTTP_MAX_BODY) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    size_t received = 0;
    while (received < len) {
        int ret = httpd_req_recv(req, (char *)buf + received, len - received);
        if (ret <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    *out_buf = buf;
    *out_len = len;
    return ESP_OK;
}

static esp_err_t gw_http_send_cbor_payload(httpd_req_t *req, const uint8_t *buf, size_t len)
{
    if (!req || !buf || len == 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no data");
    }
    httpd_resp_set_type(req, "application/cbor");
    return httpd_resp_send(req, (const char *)buf, len);
}

static esp_err_t gw_action_exec_from_cbor(const uint8_t *buf, size_t len, char *err, size_t err_size)
{
    if (!buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return gw_action_exec_cbor(buf, len, err, err_size);
}

static bool cbor_map_find_val_buf(const uint8_t *buf, size_t len, const char *key, gw_cbor_slice_t *out)
{
    if (!buf || len == 0 || !key || !out) return false;
    return gw_cbor_map_find(buf, len, key, out);
}

static bool cbor_text_copy(const gw_cbor_slice_t *s, char *out, size_t out_size)
{
    if (!s || !out || out_size == 0) return false;
    const uint8_t *p = NULL;
    size_t n = 0;
    if (!gw_cbor_slice_to_text_span(s, &p, &n) || !p) return false;
    if (n + 1 > out_size) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool cbor_slice_to_u32(const gw_cbor_slice_t *s, uint32_t *out)
{
    if (!s || !out) return false;
    uint64_t uv = 0;
    if (gw_cbor_slice_to_u64(s, &uv) && uv <= 0xffffffffULL) {
        *out = (uint32_t)uv;
        return true;
    }
    int64_t iv = 0;
    if (gw_cbor_slice_to_i64(s, &iv) && iv >= 0 && iv <= 0xffffffffLL) {
        *out = (uint32_t)iv;
        return true;
    }
    return false;
}

static bool cbor_slice_to_u8(const gw_cbor_slice_t *s, uint8_t *out)
{
    uint32_t v = 0;
    if (!cbor_slice_to_u32(s, &v) || v > 0xff) return false;
    *out = (uint8_t)v;
    return true;
}

static bool cbor_array_slices(const gw_cbor_slice_t *arr, gw_cbor_slice_t **out_items, uint32_t *out_count)
{
    if (!arr || !out_items || !out_count || !arr->ptr || arr->len == 0) return false;

    gw_cbor_reader_t r;
    gw_cbor_reader_init(&r, arr->ptr, arr->len);

    uint8_t b = 0;
    if (!gw_cbor_read_u8(&r, &b)) return false;
    const uint8_t major = b >> 5;
    const uint8_t ai = b & 0x1f;
    if (major != 4) return false;

    uint64_t count = 0;
    if (!gw_cbor_read_uint_arg(&r, ai, &count)) return false;
    if (count > UINT32_MAX) return false;

    gw_cbor_slice_t *items = NULL;
    if (count > 0) {
        items = (gw_cbor_slice_t *)calloc((size_t)count, sizeof(*items));
        if (!items) return false;
    }

    for (uint64_t i = 0; i < count; i++) {
        const uint8_t *start = r.p;
        if (!gw_cbor_skip_item(&r)) {
            free(items);
            return false;
        }
        items[i].ptr = start;
        items[i].len = (size_t)(r.p - start);
    }

    *out_items = items;
    *out_count = (uint32_t)count;
    return true;
}

static const char *automation_string_at(const gw_automation_entry_t *entry, uint32_t off)
{
    if (!entry || off == 0 || off >= entry->string_table_size) {
        return "";
    }
    return entry->string_table + off;
}

static const char *automation_evt_type_to_str(uint8_t type)
{
    switch (type) {
    case GW_AUTO_EVT_ZIGBEE_COMMAND:
        return "zigbee.command";
    case GW_AUTO_EVT_ZIGBEE_ATTR_REPORT:
        return "zigbee.attr_report";
    case GW_AUTO_EVT_DEVICE_JOIN:
        return "device.join";
    case GW_AUTO_EVT_DEVICE_LEAVE:
        return "device.leave";
    default:
        return "zigbee.command";
    }
}

static const char *automation_op_to_str(uint8_t op)
{
    switch (op) {
    case GW_AUTO_OP_EQ:
        return "==";
    case GW_AUTO_OP_NE:
        return "!=";
    case GW_AUTO_OP_GT:
        return ">";
    case GW_AUTO_OP_LT:
        return "<";
    case GW_AUTO_OP_GE:
        return ">=";
    case GW_AUTO_OP_LE:
        return "<=";
    default:
        return "==";
    }
}


static esp_err_t cbor_write_hex16_key(gw_cbor_writer_t *w, const char *key, uint16_t value)
{
    if (!w || !key || value == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[8];
    int n = snprintf(buf, sizeof(buf), "0x%04x", value);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return ESP_FAIL;
    }
    esp_err_t rc = gw_cbor_writer_text(w, key);
    if (rc != ESP_OK) return rc;
    return gw_cbor_writer_text(w, buf);
}

static esp_err_t cbor_write_automation_trigger(gw_cbor_writer_t *w,
                                              const gw_auto_bin_trigger_v2_t *trigger,
                                              const gw_automation_entry_t *entry)
{
    if (!w || !trigger || !entry) return ESP_ERR_INVALID_ARG;

    uint8_t match_pairs = 0;
    if (trigger->device_uid_off) match_pairs++;
    if (trigger->endpoint) match_pairs++;
    if (trigger->event_type == GW_AUTO_EVT_ZIGBEE_COMMAND) {
        if (trigger->cmd_off) match_pairs++;
        if (trigger->cluster_id) match_pairs++;
    } else if (trigger->event_type == GW_AUTO_EVT_ZIGBEE_ATTR_REPORT) {
        if (trigger->cluster_id) match_pairs++;
        if (trigger->attr_id) match_pairs++;
    }

    esp_err_t rc = gw_cbor_writer_map(w, 3);
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "type");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "event");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "event_type");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, automation_evt_type_to_str(trigger->event_type));
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "match");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_map(w, match_pairs);
    if (rc != ESP_OK) return rc;

    if (trigger->device_uid_off) {
        rc = gw_cbor_writer_text(w, "device_uid");
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_text(w, automation_string_at(entry, trigger->device_uid_off));
        if (rc != ESP_OK) return rc;
    }
    if (trigger->endpoint) {
        rc = gw_cbor_writer_text(w, "payload.endpoint");
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_u64(w, trigger->endpoint);
        if (rc != ESP_OK) return rc;
    }
    if (trigger->event_type == GW_AUTO_EVT_ZIGBEE_COMMAND) {
        if (trigger->cmd_off) {
            rc = gw_cbor_writer_text(w, "payload.cmd");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_text(w, automation_string_at(entry, trigger->cmd_off));
            if (rc != ESP_OK) return rc;
        }
        if (trigger->cluster_id) {
            rc = cbor_write_hex16_key(w, "payload.cluster", trigger->cluster_id);
            if (rc != ESP_OK) return rc;
        }
    } else if (trigger->event_type == GW_AUTO_EVT_ZIGBEE_ATTR_REPORT) {
        if (trigger->cluster_id) {
            rc = cbor_write_hex16_key(w, "payload.cluster", trigger->cluster_id);
            if (rc != ESP_OK) return rc;
        }
        if (trigger->attr_id) {
            rc = cbor_write_hex16_key(w, "payload.attr", trigger->attr_id);
            if (rc != ESP_OK) return rc;
        }
    }
    return ESP_OK;
}

static esp_err_t cbor_write_automation_condition(gw_cbor_writer_t *w,
                                                const gw_auto_bin_condition_v2_t *cond,
                                                const gw_automation_entry_t *entry)
{
    if (!w || !cond || !entry) return ESP_ERR_INVALID_ARG;

    esp_err_t rc = gw_cbor_writer_map(w, 4);
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "type");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "state");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "op");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, automation_op_to_str(cond->op));
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "ref");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_map(w, 2);
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "device_uid");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, automation_string_at(entry, cond->device_uid_off));
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "key");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, automation_string_at(entry, cond->key_off));
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "value");
    if (rc != ESP_OK) return rc;
    if (cond->val_type == GW_AUTO_VAL_BOOL) {
        rc = gw_cbor_writer_bool(w, cond->v.b != 0);
    } else {
        rc = gw_cbor_writer_f64(w, cond->v.f64);
    }
    return rc;
}

static esp_err_t cbor_write_automation_action(gw_cbor_writer_t *w,
                                             const gw_auto_bin_action_v2_t *action,
                                             const gw_automation_entry_t *entry)
{
    if (!w || !action || !entry) return ESP_ERR_INVALID_ARG;
    const char *cmd = automation_string_at(entry, action->cmd_off);

    uint8_t pairs = 2; // type, cmd
    if (action->kind == GW_AUTO_ACT_BIND) {
        pairs += 5;
    } else if (action->kind == GW_AUTO_ACT_SCENE) {
        pairs += 2;
    } else if (action->kind == GW_AUTO_ACT_GROUP) {
        pairs += 1;
        if (strcmp(cmd, "level.move_to_level") == 0) pairs += 2;
        else if (strcmp(cmd, "color.move_to_color_xy") == 0) pairs += 3;
        else if (strcmp(cmd, "color.move_to_color_temperature") == 0) pairs += 2;
    } else if (action->kind == GW_AUTO_ACT_DEVICE) {
        pairs += 2;
        if (strcmp(cmd, "level.move_to_level") == 0) pairs += 2;
        else if (strcmp(cmd, "color.move_to_color_xy") == 0) pairs += 3;
        else if (strcmp(cmd, "color.move_to_color_temperature") == 0) pairs += 2;
    }

    esp_err_t rc = gw_cbor_writer_map(w, pairs);
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "type");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "zigbee");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "cmd");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, cmd);
    if (rc != ESP_OK) return rc;

    if (action->kind == GW_AUTO_ACT_BIND) {
        rc = gw_cbor_writer_text(w, "src_device_uid");
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_text(w, automation_string_at(entry, action->uid_off));
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_text(w, "src_endpoint");
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_u64(w, action->endpoint);
        if (rc != ESP_OK) return rc;
        rc = cbor_write_hex16_key(w, "cluster_id", action->u16_0);
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_text(w, "dst_device_uid");
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_text(w, automation_string_at(entry, action->uid2_off));
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_text(w, "dst_endpoint");
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_u64(w, action->aux_ep);
        if (rc != ESP_OK) return rc;
        return ESP_OK;
    }

    if (action->kind == GW_AUTO_ACT_SCENE) {
        rc = cbor_write_hex16_key(w, "group_id", action->u16_0);
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_text(w, "scene_id");
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_u64(w, action->u16_1);
        return rc;
    }

    if (action->kind == GW_AUTO_ACT_GROUP) {
        rc = cbor_write_hex16_key(w, "group_id", action->u16_0);
        if (rc != ESP_OK) return rc;
        if (strcmp(cmd, "level.move_to_level") == 0) {
            rc = gw_cbor_writer_text(w, "level");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg0_u32);
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_text(w, "transition_ms");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg1_u32);
            return rc;
        }
        if (strcmp(cmd, "color.move_to_color_xy") == 0) {
            rc = gw_cbor_writer_text(w, "x");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg0_u32);
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_text(w, "y");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg1_u32);
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_text(w, "transition_ms");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg2_u32);
            return rc;
        }
        if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
            rc = gw_cbor_writer_text(w, "mireds");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg0_u32);
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_text(w, "transition_ms");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg1_u32);
            return rc;
        }
        return ESP_OK;
    }

    if (action->kind == GW_AUTO_ACT_DEVICE) {
        rc = gw_cbor_writer_text(w, "device_uid");
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_text(w, automation_string_at(entry, action->uid_off));
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_text(w, "endpoint");
        if (rc != ESP_OK) return rc;
        rc = gw_cbor_writer_u64(w, action->endpoint);
        if (rc != ESP_OK) return rc;
        if (strcmp(cmd, "level.move_to_level") == 0) {
            rc = gw_cbor_writer_text(w, "level");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg0_u32);
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_text(w, "transition_ms");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg1_u32);
            return rc;
        }
        if (strcmp(cmd, "color.move_to_color_xy") == 0) {
            rc = gw_cbor_writer_text(w, "x");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg0_u32);
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_text(w, "y");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg1_u32);
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_text(w, "transition_ms");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg2_u32);
            return rc;
        }
        if (strcmp(cmd, "color.move_to_color_temperature") == 0) {
            rc = gw_cbor_writer_text(w, "mireds");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg0_u32);
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_text(w, "transition_ms");
            if (rc != ESP_OK) return rc;
            rc = gw_cbor_writer_u64(w, action->arg1_u32);
            return rc;
        }
    }
    return ESP_OK;
}

static esp_err_t cbor_write_automation_definition(gw_cbor_writer_t *w, const gw_automation_entry_t *entry)
{
    if (!w || !entry) return ESP_ERR_INVALID_ARG;

    esp_err_t rc = gw_cbor_writer_map(w, 8);
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "v");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_u64(w, 1);
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "id");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, entry->id);
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "name");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, entry->name);
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "enabled");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_bool(w, entry->enabled);
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "mode");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_text(w, "single");
    if (rc != ESP_OK) return rc;

    rc = gw_cbor_writer_text(w, "triggers");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_array(w, entry->triggers_count);
    if (rc != ESP_OK) return rc;
    for (uint8_t i = 0; i < entry->triggers_count; i++) {
        rc = cbor_write_automation_trigger(w, &entry->triggers[i], entry);
        if (rc != ESP_OK) return rc;
    }

    rc = gw_cbor_writer_text(w, "conditions");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_array(w, entry->conditions_count);
    if (rc != ESP_OK) return rc;
    for (uint8_t i = 0; i < entry->conditions_count; i++) {
        rc = cbor_write_automation_condition(w, &entry->conditions[i], entry);
        if (rc != ESP_OK) return rc;
    }

    rc = gw_cbor_writer_text(w, "actions");
    if (rc != ESP_OK) return rc;
    rc = gw_cbor_writer_array(w, entry->actions_count);
    if (rc != ESP_OK) return rc;
    for (uint8_t i = 0; i < entry->actions_count; i++) {
        rc = cbor_write_automation_action(w, &entry->actions[i], entry);
        if (rc != ESP_OK) return rc;
    }

    return ESP_OK;
}

static esp_err_t api_devices_flatbuffer_get_handler(httpd_req_t *req)
{
    static int64_t s_last_fb_sync_us = 0;
    size_t len = 0;
    uint8_t *buf = NULL;
    if (gw_device_fb_store_copy(&buf, &len) != ESP_OK || !buf || len == 0) {
        if (buf) {
            free(buf);
            buf = NULL;
        }
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_fb_sync_us > 1000000) {
            s_last_fb_sync_us = now_us;
            (void)gw_zigbee_sync_device_fb();
        }
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_hdr(req, "Retry-After", "1");
        httpd_resp_sendstr(req, "device flatbuffer is syncing");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Device-Buffer-Format", "flatbuffer");
    esp_err_t err = httpd_resp_send(req, (const char *)buf, (ssize_t)len);
    free(buf);
    return err;
}

static esp_err_t api_devices_post_handler(httpd_req_t *req)
{
    // Body (CBOR): { device_uid: string, name: string }
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gw_http_recv_body(req, &buf, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cbor");
        return ESP_OK;
    }

    gw_cbor_slice_t uid_s = {0};
    if (!cbor_map_find_val_buf(buf, len, "device_uid", &uid_s)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing device_uid");
        return ESP_OK;
    }
    gw_device_uid_t duid = {0};
    if (!cbor_text_copy(&uid_s, duid.uid, sizeof(duid.uid))) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad device_uid");
        return ESP_OK;
    }

    gw_cbor_slice_t name_s = {0};
    if (!cbor_map_find_val_buf(buf, len, "name", &name_s)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
        return ESP_OK;
    }
    char name_buf[32] = {0};
    if (!cbor_text_copy(&name_s, name_buf, sizeof(name_buf))) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }

    esp_err_t err = gw_zigbee_set_device_name(&duid, name_buf);
    free(buf);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "device not found");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "c6 rename failed");
        }
        return ESP_OK;
    }

    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 2);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ok");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "queued");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                       : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_devices_remove_post_handler(httpd_req_t *req)
{
    // Body (CBOR): { device_uid: string }
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gw_http_recv_body(req, &buf, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cbor");
        return ESP_OK;
    }

    gw_cbor_slice_t uid_s = {0};
    if (!cbor_map_find_val_buf(buf, len, "device_uid", &uid_s)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing device_uid");
        return ESP_OK;
    }
    gw_device_uid_t uid = {0};
    if (!cbor_text_copy(&uid_s, uid.uid, sizeof(uid.uid))) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad device_uid");
        return ESP_OK;
    }

    free(buf);

    esp_err_t rm = gw_zigbee_remove_device(&uid);
    if (rm != ESP_OK) {
        if (rm == ESP_ERR_NOT_FOUND) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "device not found");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "c6 remove failed");
        }
        return ESP_OK;
    }

    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 3);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ok");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "device_uid");
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, uid.uid);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "queued");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                       : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t gw_http_send_group_store_error(httpd_req_t *req, esp_err_t err, const char *not_found_msg, const char *no_mem_msg)
{
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, not_found_msg ? not_found_msg : "not found");
    }
    if (err == ESP_ERR_NO_MEM) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, no_mem_msg ? no_mem_msg : "store full");
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
}

static esp_err_t gw_http_send_cbor_ok(httpd_req_t *req)
{
    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 1);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ok");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    esp_err_t send_err = (rc == ESP_OK)
                             ? gw_http_send_cbor_payload(req, w.buf, w.len)
                             : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_network_permit_join_post_handler(httpd_req_t *req)
{
    // Body (CBOR): { seconds?: number } (default 180)
    uint8_t seconds = 180;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gw_http_recv_body(req, &buf, &len) == ESP_OK) {
        gw_cbor_slice_t sec_s = {0};
        if (cbor_map_find_val_buf(buf, len, "seconds", &sec_s)) {
            uint8_t v = 0;
            if (cbor_slice_to_u8(&sec_s, &v) && v > 0) {
                seconds = v;
            }
        }
        free(buf);
    }

    esp_err_t err = gw_zigbee_permit_join(seconds);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "permit_join failed");
        return ESP_OK;
    }

    char msg[48];
    (void)snprintf(msg, sizeof(msg), "seconds=%u", (unsigned)seconds);
    gw_event_bus_publish("api_permit_join", "rest", "", 0, msg);

    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 2);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ok");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "seconds");
    if (rc == ESP_OK) rc = gw_cbor_writer_u64(&w, seconds);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                       : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_automations_get_handler(httpd_req_t *req)
{
    const size_t max_autos = 32;
    gw_automation_meta_t *metas = (gw_automation_meta_t *)calloc(max_autos, sizeof(gw_automation_meta_t));
    if (!metas) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t count = gw_automation_store_list_meta(metas, max_autos);
    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 1);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "automations");
    if (rc == ESP_OK) rc = gw_cbor_writer_array(&w, count);
    if (rc == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            const gw_automation_meta_t *meta = &metas[i];
            gw_automation_entry_t entry = {0};
            rc = gw_automation_store_get(meta->id, &entry);
            if (rc != ESP_OK) break;

            rc = gw_cbor_writer_map(&w, 4);
            if (rc != ESP_OK) break;
            rc = gw_cbor_writer_text(&w, "id");
            if (rc != ESP_OK) break;
            rc = gw_cbor_writer_text(&w, entry.id);
            if (rc != ESP_OK) break;
            rc = gw_cbor_writer_text(&w, "name");
            if (rc != ESP_OK) break;
            rc = gw_cbor_writer_text(&w, entry.name);
            if (rc != ESP_OK) break;
            rc = gw_cbor_writer_text(&w, "enabled");
            if (rc != ESP_OK) break;
            rc = gw_cbor_writer_bool(&w, entry.enabled);
            if (rc != ESP_OK) break;
            rc = gw_cbor_writer_text(&w, "automation");
            if (rc != ESP_OK) break;
            rc = cbor_write_automation_definition(&w, &entry);
            if (rc != ESP_OK) break;
        }
    }
    free(metas);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                       : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_automation_detail_patch_handler(httpd_req_t *req)
{
    char id_buf[GW_AUTOMATION_ID_MAX] = {0};
    if (!gw_http_extract_id(req->uri, "/api/automations", id_buf, sizeof(id_buf))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing automation id");
        return ESP_OK;
    }
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gw_http_recv_body(req, &buf, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cbor");
        return ESP_OK;
    }
    gw_cbor_slice_t enabled_s = {0};
    bool enabled = false;
    if (!cbor_map_find_val_buf(buf, len, "enabled", &enabled_s) || !gw_cbor_slice_to_bool(&enabled_s, &enabled)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing enabled");
        return ESP_OK;
    }
    esp_err_t err = gw_automation_store_set_enabled(id_buf, enabled);
    free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "automation not found");
        return ESP_OK;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "id=%s enabled=%s", id_buf, enabled ? "1" : "0");
    gw_event_bus_publish("automation_enabled", "rest", "", 0, msg);
    gw_event_bus_publish("automation.changed", "rest", "", 0, "enabled");
    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 1);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ok");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                       : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_automation_detail_delete_handler(httpd_req_t *req)
{
    char id_buf[GW_AUTOMATION_ID_MAX] = {0};
    if (!gw_http_extract_id(req->uri, "/api/automations", id_buf, sizeof(id_buf))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing automation id");
        return ESP_OK;
    }
    esp_err_t err = gw_automation_store_remove(id_buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "automation not found");
        return ESP_OK;
    }
    gw_event_bus_publish("automation_removed", "rest", "", 0, id_buf);
    gw_event_bus_publish("automation.changed", "rest", "", 0, "removed");
    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 1);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ok");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                       : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_automation_post_handler(httpd_req_t *req)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gw_http_recv_body(req, &buf, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cbor");
        return ESP_OK;
    }
    gw_cbor_slice_t auto_s = {0};
    if (!cbor_map_find_val_buf(buf, len, "automation", &auto_s)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing automation object");
        return ESP_OK;
    }

    // Single standard: metadata берём только из automation (верхний уровень игнорируем).
    char id_buf[GW_AUTOMATION_ID_MAX] = {0};
    bool enable = true;
    gw_cbor_slice_t id_s = {0};
    if (cbor_map_find_val_buf(auto_s.ptr, auto_s.len, "id", &id_s)) {
        (void)cbor_text_copy(&id_s, id_buf, sizeof(id_buf));
    }
    gw_cbor_slice_t enabled_s = {0};
    if (cbor_map_find_val_buf(auto_s.ptr, auto_s.len, "enabled", &enabled_s)) {
        (void)gw_cbor_slice_to_bool(&enabled_s, &enable);
    }
    if (id_buf[0] == '\0') {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing automation.id");
        return ESP_OK;
    }

    esp_err_t err = gw_automation_store_put_cbor(auto_s.ptr, auto_s.len);
    free(buf);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_OK;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "id=%s enabled=%u", id_buf, enable ? 1U : 0U);
    gw_event_bus_publish("automation_saved", "rest", "", 0, msg);
    gw_event_bus_publish("automation.changed", "rest", "", 0, "saved");
    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 1);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ok");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                       : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_actions_post_handler(httpd_req_t *req)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gw_http_recv_body(req, &buf, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cbor");
        return ESP_OK;
    }
    gw_cbor_slice_t action_s = {0};
    gw_cbor_slice_t actions_s = {0};
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cbor_map_find_val_buf(buf, len, "action", &action_s)) {
        char errbuf[128] = {0};
        err = gw_action_exec_from_cbor(action_s.ptr, action_s.len, errbuf, sizeof(errbuf));
        if (err != ESP_OK) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, errbuf[0] ? errbuf : "action failed");
            return ESP_OK;
        }
    } else if (cbor_map_find_val_buf(buf, len, "actions", &actions_s)) {
        gw_cbor_slice_t *items = NULL;
        uint32_t count = 0;
        if (!cbor_array_slices(&actions_s, &items, &count)) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "actions must be array");
            return ESP_OK;
        }
        for (uint32_t i = 0; i < count; i++) {
            char errbuf[128] = {0};
            err = gw_action_exec_from_cbor(items[i].ptr, items[i].len, errbuf, sizeof(errbuf));
            if (err != ESP_OK) {
                free(items);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, errbuf[0] ? errbuf : "action failed");
                return ESP_OK;
            }
        }
        free(items);
    } else {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing action/actions");
        return ESP_OK;
    }
    free(buf);
    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 1);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ok");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                       : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_state_get_handler(httpd_req_t *req)
{
    static const size_t kMaxDevices = 64;
    static const size_t kMaxStateItems = 1024;

    gw_device_t *devices = (gw_device_t *)calloc(kMaxDevices, sizeof(gw_device_t));
    gw_state_item_t *items = (gw_state_item_t *)calloc(kMaxStateItems, sizeof(gw_state_item_t));
    if (!devices || !items) {
        free(devices);
        free(items);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    const size_t dev_count = gw_device_registry_list(devices, kMaxDevices);
    size_t total = 0;
    for (size_t i = 0; i < dev_count && total < kMaxStateItems; i++) {
        const size_t left = kMaxStateItems - total;
        total += gw_state_store_list_uid(&devices[i].device_uid, &items[total], left);
    }

    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 1);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "items");
    if (rc == ESP_OK) rc = gw_cbor_writer_array(&w, total);
    for (size_t i = 0; rc == ESP_OK && i < total; i++) {
        rc = gw_cbor_writer_map(&w, 5);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "device_id");
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, items[i].uid.uid);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "endpoint_id");
        if (rc == ESP_OK) rc = gw_cbor_writer_u64(&w, items[i].endpoint);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "key");
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, items[i].key);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "value");
        if (rc == ESP_OK) {
            switch (items[i].value_type) {
                case GW_STATE_VALUE_BOOL:
                    rc = gw_cbor_writer_bool(&w, items[i].value_bool);
                    break;
                case GW_STATE_VALUE_F32:
                    rc = gw_cbor_writer_f64(&w, (double)items[i].value_f32);
                    break;
                case GW_STATE_VALUE_U32:
                    rc = gw_cbor_writer_u64(&w, items[i].value_u32);
                    break;
                case GW_STATE_VALUE_U64:
                    rc = gw_cbor_writer_u64(&w, items[i].value_u64);
                    break;
                default:
                    rc = gw_cbor_writer_null(&w);
                    break;
            }
        }
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ts_ms");
        if (rc == ESP_OK) rc = gw_cbor_writer_u64(&w, items[i].ts_ms);
    }

    free(devices);
    free(items);

    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                        : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_groups_get_handler(httpd_req_t *req)
{
    const size_t max_groups = 24;
    gw_group_entry_t *groups = (gw_group_entry_t *)calloc(max_groups, sizeof(gw_group_entry_t));
    if (!groups) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    const size_t count = gw_group_store_list(groups, max_groups);

    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 1);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "groups");
    if (rc == ESP_OK) rc = gw_cbor_writer_array(&w, count);
    for (size_t i = 0; rc == ESP_OK && i < count; i++) {
        rc = gw_cbor_writer_map(&w, 4);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "id");
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, groups[i].id);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "name");
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, groups[i].name);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "created_at_ms");
        if (rc == ESP_OK) rc = gw_cbor_writer_u64(&w, groups[i].created_at_ms);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "updated_at_ms");
        if (rc == ESP_OK) rc = gw_cbor_writer_u64(&w, groups[i].updated_at_ms);
    }

    free(groups);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                        : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_groups_post_handler(httpd_req_t *req)
{
    // CBOR: { op: "create"|"rename"|"delete", id?: string, name?: string }
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gw_http_recv_body(req, &buf, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cbor");
        return ESP_OK;
    }

    gw_cbor_slice_t op_s = {0};
    char op[16] = {0};
    if (!cbor_map_find_val_buf(buf, len, "op", &op_s) || !cbor_text_copy(&op_s, op, sizeof(op))) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing op");
        return ESP_OK;
    }

    gw_cbor_slice_t id_s = {0};
    gw_cbor_slice_t name_s = {0};
    char id[GW_GROUP_ID_MAX] = {0};
    char name[GW_GROUP_NAME_MAX] = {0};
    if (cbor_map_find_val_buf(buf, len, "id", &id_s)) (void)cbor_text_copy(&id_s, id, sizeof(id));
    if (cbor_map_find_val_buf(buf, len, "name", &name_s)) (void)cbor_text_copy(&name_s, name, sizeof(name));

    esp_err_t err = ESP_ERR_INVALID_ARG;
    gw_group_entry_t created = {0};
    if (strcmp(op, "create") == 0) {
        err = gw_group_store_create(id[0] ? id : NULL, name, &created);
    } else if (strcmp(op, "rename") == 0) {
        err = gw_group_store_rename(id, name);
    } else if (strcmp(op, "delete") == 0) {
        err = gw_group_store_remove(id);
    } else {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported op");
        return ESP_OK;
    }
    free(buf);

    if (err != ESP_OK) {
        gw_http_send_group_store_error(req, err, "group not found", "group store full");
        return ESP_OK;
    }

    gw_event_bus_publish("group.changed", "rest", "", 0, op);

    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 3);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "ok");
    if (rc == ESP_OK) rc = gw_cbor_writer_bool(&w, true);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "op");
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, op);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "id");
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, strcmp(op, "create") == 0 ? created.id : id);
    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                        : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_group_items_get_handler(httpd_req_t *req)
{
    const size_t max_items = 256;
    gw_group_item_t *items = (gw_group_item_t *)calloc(max_items, sizeof(gw_group_item_t));
    if (!items) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    const size_t count = gw_group_store_list_items(items, max_items);

    gw_cbor_writer_t w;
    gw_cbor_writer_init(&w);
    esp_err_t rc = gw_cbor_writer_map(&w, 1);
    if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "items");
    if (rc == ESP_OK) rc = gw_cbor_writer_array(&w, count);
    for (size_t i = 0; rc == ESP_OK && i < count; i++) {
        rc = gw_cbor_writer_map(&w, 5);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "group_id");
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, items[i].group_id);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "device_uid");
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, items[i].device_uid.uid);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "endpoint_id");
        if (rc == ESP_OK) rc = gw_cbor_writer_u64(&w, items[i].endpoint);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "order");
        if (rc == ESP_OK) rc = gw_cbor_writer_u64(&w, items[i].order);
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, "label");
        if (rc == ESP_OK) rc = gw_cbor_writer_text(&w, items[i].label);
    }
    free(items);

    esp_err_t send_err = (rc == ESP_OK) ? gw_http_send_cbor_payload(req, w.buf, w.len)
                                        : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cbor encode failure");
    gw_cbor_writer_free(&w);
    return send_err;
}

static esp_err_t api_group_items_post_handler(httpd_req_t *req)
{
    // CBOR:
    // { op: "set"|"remove"|"reorder"|"label", group_id?: string, device_uid: string, endpoint_id: number, order?: number, label?: string }
    uint8_t *buf = NULL;
    size_t len = 0;
    if (gw_http_recv_body(req, &buf, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cbor");
        return ESP_OK;
    }

    gw_cbor_slice_t op_s = {0};
    char op[16] = {0};
    if (!cbor_map_find_val_buf(buf, len, "op", &op_s) || !cbor_text_copy(&op_s, op, sizeof(op))) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing op");
        return ESP_OK;
    }

    gw_cbor_slice_t uid_s = {0};
    gw_cbor_slice_t ep_s = {0};
    if (!cbor_map_find_val_buf(buf, len, "device_uid", &uid_s) || !cbor_map_find_val_buf(buf, len, "endpoint_id", &ep_s)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing device_uid/endpoint_id");
        return ESP_OK;
    }

    gw_device_uid_t uid = {0};
    uint8_t endpoint = 0;
    if (!cbor_text_copy(&uid_s, uid.uid, sizeof(uid.uid)) || !cbor_slice_to_u8(&ep_s, &endpoint) || endpoint == 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad device_uid/endpoint_id");
        return ESP_OK;
    }

    gw_cbor_slice_t group_s = {0};
    char group_id[GW_GROUP_ID_MAX] = {0};
    if (cbor_map_find_val_buf(buf, len, "group_id", &group_s)) {
        (void)cbor_text_copy(&group_s, group_id, sizeof(group_id));
    }

    gw_cbor_slice_t order_s = {0};
    uint32_t order = 0;
    bool has_order = false;
    if (cbor_map_find_val_buf(buf, len, "order", &order_s)) {
        has_order = cbor_slice_to_u32(&order_s, &order);
    }
    gw_cbor_slice_t label_s = {0};
    char label[32] = {0};
    const bool has_label = cbor_map_find_val_buf(buf, len, "label", &label_s) && cbor_text_copy(&label_s, label, sizeof(label));

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (strcmp(op, "set") == 0) {
        err = gw_group_store_set_endpoint(group_id, &uid, endpoint);
        if (err == ESP_OK && has_label) {
            err = gw_group_store_set_endpoint_label(&uid, endpoint, label);
        }
    } else if (strcmp(op, "remove") == 0) {
        err = gw_group_store_remove_endpoint(&uid, endpoint);
    } else if (strcmp(op, "reorder") == 0) {
        if (!has_order || order == 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing/bad order");
            return ESP_OK;
        }
        err = gw_group_store_reorder_endpoint(group_id, &uid, endpoint, order);
    } else if (strcmp(op, "label") == 0) {
        err = gw_group_store_set_endpoint_label(&uid, endpoint, label);
    } else {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported op");
        return ESP_OK;
    }
    free(buf);

    if (err != ESP_OK) {
        gw_http_send_group_store_error(req, err, "item not found", "group items full");
        return ESP_OK;
    }

    gw_event_bus_publish("group.changed", "rest", "", 0, op);

    return gw_http_send_cbor_ok(req);
}

esp_err_t gw_http_register_rest_endpoints(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }

    static const httpd_uri_t api_devices_flatbuffer_get_uri = {
        .uri = "/api/devices/flatbuffer",
        .method = HTTP_GET,
        .handler = api_devices_flatbuffer_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_devices_post_uri = {
        .uri = "/api/devices",
        .method = HTTP_POST,
        .handler = api_devices_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_automations_get_uri = {
        .uri = "/api/automations",
        .method = HTTP_GET,
        .handler = api_automations_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_automations_post_uri = {
        .uri = "/api/automations",
        .method = HTTP_POST,
        .handler = api_automation_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_automations_detail_patch_uri = {
        .uri = "/api/automations/*",
        .method = HTTP_PATCH,
        .handler = api_automation_detail_patch_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_automations_detail_delete_uri = {
        .uri = "/api/automations/*",
        .method = HTTP_DELETE,
        .handler = api_automation_detail_delete_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_actions_post_uri = {
        .uri = "/api/actions",
        .method = HTTP_POST,
        .handler = api_actions_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_state_get_uri = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = api_state_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_devices_remove_post_uri = {
        .uri = "/api/devices/remove",
        .method = HTTP_POST,
        .handler = api_devices_remove_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_network_permit_join_post_uri = {
        .uri = "/api/network/permit_join",
        .method = HTTP_POST,
        .handler = api_network_permit_join_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_groups_get_uri = {
        .uri = "/api/groups",
        .method = HTTP_GET,
        .handler = api_groups_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_groups_post_uri = {
        .uri = "/api/groups",
        .method = HTTP_POST,
        .handler = api_groups_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_group_items_get_uri = {
        .uri = "/api/groups/items",
        .method = HTTP_GET,
        .handler = api_group_items_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_group_items_post_uri = {
        .uri = "/api/groups/items",
        .method = HTTP_POST,
        .handler = api_group_items_post_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(server, &api_devices_flatbuffer_get_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_devices_post_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_automations_get_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_automations_post_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_automations_detail_patch_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_automations_detail_delete_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_actions_post_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_state_get_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_devices_remove_post_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_network_permit_join_post_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_groups_get_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_groups_post_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_group_items_get_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &api_group_items_post_uri);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}


