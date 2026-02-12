#include "gw_core/automation_compiled.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_core/cbor.h"
#include "esp_log.h"

#define MAGIC_GWAR 0x52415747u // 'GWAR'

static void set_err(char *out, size_t out_size, const char *msg)
{
    if (!out || out_size == 0) return;
    if (!msg) {
        out[0] = '\0';
        return;
    }
    strncpy(out, msg, out_size);
    out[out_size - 1] = '\0';
}

static uint16_t parse_u16_any_cbor(const gw_cbor_slice_t *s, bool *ok)
{
    if (ok) *ok = false;
    if (!s || !s->ptr || s->len == 0) return 0;

    int64_t iv = 0;
    if (gw_cbor_slice_to_i64(s, &iv)) {
        if (iv >= 0 && iv <= 65535) {
            if (ok) *ok = true;
            return (uint16_t)iv;
        }
        return 0;
    }

    const uint8_t *p = NULL;
    size_t n = 0;
    if (gw_cbor_slice_to_text_span(s, &p, &n) && p && n) {
        char tmp[16];
        if (n >= sizeof(tmp)) return 0;
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        char *end = NULL;
        unsigned long v = strtoul(tmp, &end, 0);
        if (end && *end == '\0' && v <= 65535UL) {
            if (ok) *ok = true;
            return (uint16_t)v;
        }
    }
    return 0;
}

static uint32_t parse_u32_any_cbor(const gw_cbor_slice_t *s, bool *ok)
{
    if (ok) *ok = false;
    if (!s || !s->ptr || s->len == 0) return 0;

    int64_t iv = 0;
    if (gw_cbor_slice_to_i64(s, &iv)) {
        if (iv >= 0 && iv <= 0x7fffffff) {
            if (ok) *ok = true;
            return (uint32_t)iv;
        }
        uint64_t uv = 0;
        if (gw_cbor_slice_to_u64(s, &uv) && uv <= 0xffffffffULL) {
            if (ok) *ok = true;
            return (uint32_t)uv;
        }
        return 0;
    }

    uint64_t uv = 0;
    if (gw_cbor_slice_to_u64(s, &uv) && uv <= 0xffffffffULL) {
        if (ok) *ok = true;
        return (uint32_t)uv;
    }

    const uint8_t *p = NULL;
    size_t n = 0;
    if (gw_cbor_slice_to_text_span(s, &p, &n) && p && n) {
        char tmp[24];
        if (n >= sizeof(tmp)) return 0;
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        char *end = NULL;
        unsigned long v = strtoul(tmp, &end, 0);
        if (end && *end == '\0') {
            if (ok) *ok = true;
            return (uint32_t)v;
        }
    }
    return 0;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} strtab_t;

static esp_err_t strtab_init(strtab_t *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    t->buf = (char *)calloc(1, 1);
    if (!t->buf) return ESP_ERR_NO_MEM;
    t->len = 1; // offset 0 => ""
    t->cap = 1;
    return ESP_OK;
}

static void strtab_free(strtab_t *t)
{
    if (!t) return;
    free(t->buf);
    *t = (strtab_t){0};
}

static uint32_t strtab_add_n(strtab_t *t, const uint8_t *s, size_t n)
{
    if (!t || !t->buf || !s || n == 0) return 0;

    // de-dupe: linear scan
    for (size_t off = 0; off < t->len;) {
        const char *cur = t->buf + off;
        size_t cur_n = strlen(cur);
        if (cur_n == n && memcmp(cur, s, n) == 0) {
            return (uint32_t)off;
        }
        off += cur_n + 1;
    }

    const size_t add_n = n + 1;
    if (t->len + add_n > t->cap) {
        size_t next = t->cap ? t->cap : 1;
        while (next < t->len + add_n) next *= 2;
        char *nb = (char *)realloc(t->buf, next);
        if (!nb) return 0;
        t->buf = nb;
        t->cap = next;
    }
    uint32_t off = (uint32_t)t->len;
    memcpy(t->buf + t->len, s, n);
    t->buf[t->len + n] = '\0';
    t->len += add_n;
    return off;
}

static gw_auto_evt_type_t evt_type_from_str(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "zigbee.command") == 0) return GW_AUTO_EVT_ZIGBEE_COMMAND;
    if (strcmp(s, "zigbee.attr_report") == 0) return GW_AUTO_EVT_ZIGBEE_ATTR_REPORT;
    if (strcmp(s, "device.join") == 0) return GW_AUTO_EVT_DEVICE_JOIN;
    if (strcmp(s, "device.leave") == 0) return GW_AUTO_EVT_DEVICE_LEAVE;
    return 0;
}

static gw_auto_op_t op_from_str(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "==") == 0) return GW_AUTO_OP_EQ;
    if (strcmp(s, "!=") == 0) return GW_AUTO_OP_NE;
    if (strcmp(s, ">") == 0) return GW_AUTO_OP_GT;
    if (strcmp(s, "<") == 0) return GW_AUTO_OP_LT;
    if (strcmp(s, ">=") == 0) return GW_AUTO_OP_GE;
    if (strcmp(s, "<=") == 0) return GW_AUTO_OP_LE;
    return 0;
}

static bool cbor_text_equals(const gw_cbor_slice_t *s, const char *lit)
{
    if (!s || !lit) return false;
    const uint8_t *p = NULL;
    size_t n = 0;
    if (!gw_cbor_slice_to_text_span(s, &p, &n) || !p) return false;
    const size_t ln = strlen(lit);
    return n == ln && memcmp(p, lit, ln) == 0;
}

static bool cbor_get_text_span(const gw_cbor_slice_t *s, const uint8_t **out_p, size_t *out_n);

static bool is_valid_uid_span(const uint8_t *p, size_t n)
{
    if (!p || n != 18) return false;
    if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) return false;
    for (size_t i = 2; i < n; i++) {
        if (!isxdigit(p[i])) return false;
    }
    return true;
}

static bool cbor_text_is_uid(const gw_cbor_slice_t *s)
{
    const uint8_t *p = NULL;
    size_t n = 0;
    if (!cbor_get_text_span(s, &p, &n) || !p || n == 0) return false;
    return is_valid_uid_span(p, n);
}

static bool cbor_map_find_val(const gw_cbor_slice_t *map, const char *key, gw_cbor_slice_t *out)
{
    if (!map || !map->ptr || map->len == 0 || !key || !out) return false;
    return gw_cbor_map_find(map->ptr, map->len, key, out);
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

static bool cbor_is_map(const gw_cbor_slice_t *s)
{
    uint64_t pairs = 0;
    return s && s->ptr && s->len && gw_cbor_top_is_map(s->ptr, s->len, &pairs);
}

static bool cbor_get_text_span(const gw_cbor_slice_t *s, const uint8_t **out_p, size_t *out_n)
{
    if (out_p) *out_p = NULL;
    if (out_n) *out_n = 0;
    if (!s || !s->ptr || s->len == 0) return false;
    return gw_cbor_slice_to_text_span(s, out_p, out_n);
}

static bool cbor_text_to_strtab(const gw_cbor_slice_t *s, strtab_t *st, uint32_t *out_off)
{
    const uint8_t *p = NULL;
    size_t n = 0;
    if (!cbor_get_text_span(s, &p, &n) || !p || n == 0) return false;
    uint32_t off = strtab_add_n(st, p, n);
    if (!off) return false;
    if (out_off) *out_off = off;
    return true;
}

static bool cbor_slice_to_double(const gw_cbor_slice_t *s, double *out)
{
    if (!s || !out) return false;
    double dv = 0.0;
    if (gw_cbor_slice_to_f64(s, &dv)) {
        *out = dv;
        return true;
    }
    int64_t iv = 0;
    if (gw_cbor_slice_to_i64(s, &iv)) {
        *out = (double)iv;
        return true;
    }
    uint64_t uv = 0;
    if (gw_cbor_slice_to_u64(s, &uv)) {
        *out = (double)uv;
        return true;
    }
    return false;
}

static esp_err_t compile_triggers(const gw_cbor_slice_t *trigger_items,
                                  uint32_t trigger_count,
                                  gw_auto_bin_trigger_v2_t *trigs,
                                  strtab_t *st,
                                  char *err,
                                  size_t err_size)
{
    if (trigger_count == 0) return ESP_OK;
    if (!trigger_items || !trigs || !st) return ESP_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < trigger_count; i++) {
        const gw_cbor_slice_t *t = &trigger_items[i];
        if (!cbor_is_map(t)) {
            set_err(err, err_size, "trigger must be object");
            return ESP_ERR_INVALID_ARG;
        }

        gw_cbor_slice_t type_s = {0};
        gw_cbor_slice_t event_type_s = {0};
        gw_cbor_slice_t match_s = {0};
        if (!cbor_map_find_val(t, "type", &type_s) || !cbor_text_equals(&type_s, "event")) {
            set_err(err, err_size, "unsupported trigger.type");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_map_find_val(t, "event_type", &event_type_s)) {
            set_err(err, err_size, "missing trigger.event_type");
            return ESP_ERR_INVALID_ARG;
        }
        const uint8_t *ev_p = NULL;
        size_t ev_n = 0;
        if (!cbor_get_text_span(&event_type_s, &ev_p, &ev_n) || !ev_p) {
            set_err(err, err_size, "missing trigger.event_type");
            return ESP_ERR_INVALID_ARG;
        }
        char ev_buf[32];
        if (ev_n >= sizeof(ev_buf)) {
            set_err(err, err_size, "bad event_type");
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(ev_buf, ev_p, ev_n);
        ev_buf[ev_n] = '\0';
        gw_auto_evt_type_t et = evt_type_from_str(ev_buf);
        if (!et) {
            set_err(err, err_size, "unsupported event_type");
            return ESP_ERR_INVALID_ARG;
        }

        trigs[i].event_type = (uint8_t)et;
        trigs[i].endpoint = 0;
        trigs[i].device_uid_off = 0;
        trigs[i].cmd_off = 0;
        trigs[i].cluster_id = 0;
        trigs[i].attr_id = 0;

        if (cbor_map_find_val(t, "match", &match_s) && cbor_is_map(&match_s)) {
            gw_cbor_slice_t uid_m = {0};
            if (cbor_map_find_val(&match_s, "device_uid", &uid_m)) {
                if (!cbor_text_is_uid(&uid_m)) {
                    set_err(err, err_size, "bad trigger.device_uid");
                    return ESP_ERR_INVALID_ARG;
                }
                uint32_t off = 0;
                if (cbor_text_to_strtab(&uid_m, st, &off)) {
                    trigs[i].device_uid_off = off;
                }
            }

            gw_cbor_slice_t ep_m = {0};
            if (cbor_map_find_val(&match_s, "payload.endpoint", &ep_m)) {
                bool ok16 = false;
                uint16_t v = parse_u16_any_cbor(&ep_m, &ok16);
                if (ok16 && v <= 240) {
                    trigs[i].endpoint = (uint8_t)v;
                }
            }

            if (et == GW_AUTO_EVT_ZIGBEE_COMMAND) {
                gw_cbor_slice_t cmd_m = {0};
                if (cbor_map_find_val(&match_s, "payload.cmd", &cmd_m)) {
                    uint32_t off = 0;
                    if (cbor_text_to_strtab(&cmd_m, st, &off)) {
                        trigs[i].cmd_off = off;
                    }
                }
                gw_cbor_slice_t cluster_m = {0};
                if (cbor_map_find_val(&match_s, "payload.cluster", &cluster_m)) {
                    bool ok16 = false;
                    uint16_t cid = parse_u16_any_cbor(&cluster_m, &ok16);
                    if (ok16) trigs[i].cluster_id = cid;
                }
            } else if (et == GW_AUTO_EVT_ZIGBEE_ATTR_REPORT) {
                gw_cbor_slice_t cluster_m = {0};
                gw_cbor_slice_t attr_m = {0};
                bool okc = false;
                bool oka = false;
                if (cbor_map_find_val(&match_s, "payload.cluster", &cluster_m)) {
                    uint16_t cid = parse_u16_any_cbor(&cluster_m, &okc);
                    if (okc) trigs[i].cluster_id = cid;
                }
                if (cbor_map_find_val(&match_s, "payload.attr", &attr_m)) {
                    uint16_t aid = parse_u16_any_cbor(&attr_m, &oka);
                    if (oka) trigs[i].attr_id = aid;
                }
            }
        }
    }

    return ESP_OK;
}

static esp_err_t compile_conditions(const gw_cbor_slice_t *cond_items,
                                    uint32_t cond_count,
                                    gw_auto_bin_condition_v2_t *conds,
                                    strtab_t *st,
                                    char *err,
                                    size_t err_size)
{
    if (cond_count == 0) return ESP_OK;
    if (!cond_items || !conds || !st) return ESP_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < cond_count; i++) {
        const gw_cbor_slice_t *c = &cond_items[i];
        if (!cbor_is_map(c)) {
            set_err(err, err_size, "condition must be object");
            return ESP_ERR_INVALID_ARG;
        }
        gw_cbor_slice_t type_s = {0};
        gw_cbor_slice_t op_s = {0};
        gw_cbor_slice_t ref_s = {0};
        gw_cbor_slice_t value_s = {0};
        if (!cbor_map_find_val(c, "type", &type_s) || !cbor_text_equals(&type_s, "state")) {
            set_err(err, err_size, "unsupported condition.type");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_map_find_val(c, "op", &op_s)) {
            set_err(err, err_size, "missing condition.op");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_map_find_val(c, "ref", &ref_s) || !cbor_is_map(&ref_s)) {
            set_err(err, err_size, "missing condition.ref");
            return ESP_ERR_INVALID_ARG;
        }
        gw_cbor_slice_t uid_s = {0};
        gw_cbor_slice_t key_s = {0};
        if (!cbor_map_find_val(&ref_s, "device_uid", &uid_s) || !uid_s.ptr) {
            set_err(err, err_size, "missing condition.ref.device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_text_is_uid(&uid_s)) {
            set_err(err, err_size, "bad condition.ref.device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_map_find_val(&ref_s, "key", &key_s) || !key_s.ptr) {
            set_err(err, err_size, "missing condition.ref.key");
            return ESP_ERR_INVALID_ARG;
        }

        const uint8_t *op_p = NULL;
        size_t op_n = 0;
        if (!cbor_get_text_span(&op_s, &op_p, &op_n) || !op_p) {
            set_err(err, err_size, "missing condition.op");
            return ESP_ERR_INVALID_ARG;
        }
        char op_buf[8];
        if (op_n >= sizeof(op_buf)) {
            set_err(err, err_size, "bad condition.op");
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(op_buf, op_p, op_n);
        op_buf[op_n] = '\0';
        gw_auto_op_t op = op_from_str(op_buf);
        if (!op) {
            set_err(err, err_size, "bad condition.op");
            return ESP_ERR_INVALID_ARG;
        }

        conds[i].op = (uint8_t)op;
        if (!cbor_text_to_strtab(&uid_s, st, &conds[i].device_uid_off)) {
            set_err(err, err_size, "bad condition.ref.device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_text_to_strtab(&key_s, st, &conds[i].key_off)) {
            set_err(err, err_size, "bad condition.ref.key");
            return ESP_ERR_INVALID_ARG;
        }

        if (cbor_map_find_val(c, "value", &value_s)) {
            bool vb = false;
            if (gw_cbor_slice_to_bool(&value_s, &vb)) {
                conds[i].val_type = GW_AUTO_VAL_BOOL;
                conds[i].v.b = vb ? 1 : 0;
            } else {
                double dv = 0.0;
                if (cbor_slice_to_double(&value_s, &dv)) {
                    conds[i].val_type = GW_AUTO_VAL_F64;
                    conds[i].v.f64 = dv;
                } else {
                    const uint8_t *vp = NULL;
                    size_t vn = 0;
                    if (cbor_get_text_span(&value_s, &vp, &vn) && vp && vn > 0 && vn < 32) {
                        char tmp[32];
                        memcpy(tmp, vp, vn);
                        tmp[vn] = '\0';
                        char *end = NULL;
                        double v = strtod(tmp, &end);
                        if (end && *end == '\0') {
                            conds[i].val_type = GW_AUTO_VAL_F64;
                            conds[i].v.f64 = v;
                        } else {
                            set_err(err, err_size, "bad condition.value");
                            return ESP_ERR_INVALID_ARG;
                        }
                    } else {
                        set_err(err, err_size, "bad condition.value");
                        return ESP_ERR_INVALID_ARG;
                    }
                }
            }
        } else {
            conds[i].val_type = GW_AUTO_VAL_BOOL;
            conds[i].v.b = 1;
        }
    }

    return ESP_OK;
}

static esp_err_t compile_actions(const gw_cbor_slice_t *action_items,
                                 uint32_t action_count,
                                 gw_auto_bin_action_v2_t *acts,
                                 strtab_t *st,
                                 char *err,
                                 size_t err_size)
{
    if (action_count == 0) return ESP_OK;
    if (!action_items || !acts || !st) return ESP_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < action_count; i++) {
        const gw_cbor_slice_t *a = &action_items[i];
        if (!cbor_is_map(a)) {
            set_err(err, err_size, "action must be object");
            return ESP_ERR_INVALID_ARG;
        }

        gw_cbor_slice_t type_s = {0};
        gw_cbor_slice_t cmd_s = {0};
        if (!cbor_map_find_val(a, "type", &type_s) || !cbor_text_equals(&type_s, "zigbee")) {
            set_err(err, err_size, "unsupported action.type");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_map_find_val(a, "cmd", &cmd_s)) {
            set_err(err, err_size, "missing action.cmd");
            return ESP_ERR_INVALID_ARG;
        }
        const uint8_t *cmd_p = NULL;
        size_t cmd_n = 0;
        if (!cbor_get_text_span(&cmd_s, &cmd_p, &cmd_n) || !cmd_p || cmd_n == 0) {
            set_err(err, err_size, "missing action.cmd");
            return ESP_ERR_INVALID_ARG;
        }

        acts[i].cmd_off = strtab_add_n(st, cmd_p, cmd_n);

        // 1) Binding / unbinding (ZDO)
        if (cbor_text_equals(&cmd_s, "bind") || cbor_text_equals(&cmd_s, "unbind") ||
            cbor_text_equals(&cmd_s, "bindings.bind") || cbor_text_equals(&cmd_s, "bindings.unbind")) {
            gw_cbor_slice_t src_uid_s = {0};
            gw_cbor_slice_t src_ep_s = {0};
            gw_cbor_slice_t cluster_s = {0};
            gw_cbor_slice_t dst_uid_s = {0};
            gw_cbor_slice_t dst_ep_s = {0};

            if (!cbor_map_find_val(a, "src_device_uid", &src_uid_s) || !src_uid_s.ptr) {
                set_err(err, err_size, "missing action.src_device_uid");
                return ESP_ERR_INVALID_ARG;
            }
            if (!cbor_map_find_val(a, "dst_device_uid", &dst_uid_s) || !dst_uid_s.ptr) {
                set_err(err, err_size, "missing action.dst_device_uid");
                return ESP_ERR_INVALID_ARG;
            }
            if (!cbor_text_is_uid(&src_uid_s)) {
                set_err(err, err_size, "bad action.src_device_uid");
                return ESP_ERR_INVALID_ARG;
            }
            if (!cbor_text_is_uid(&dst_uid_s)) {
                set_err(err, err_size, "bad action.dst_device_uid");
                return ESP_ERR_INVALID_ARG;
            }

            bool ok_src_ep = false;
            uint16_t src_ep = 0;
            if (cbor_map_find_val(a, "src_endpoint", &src_ep_s)) {
                src_ep = parse_u16_any_cbor(&src_ep_s, &ok_src_ep);
            }
            if (!ok_src_ep || src_ep == 0 || src_ep > 240) {
                set_err(err, err_size, "bad action.src_endpoint");
                return ESP_ERR_INVALID_ARG;
            }

            bool ok_dst_ep = false;
            uint16_t dst_ep = 0;
            if (cbor_map_find_val(a, "dst_endpoint", &dst_ep_s)) {
                dst_ep = parse_u16_any_cbor(&dst_ep_s, &ok_dst_ep);
            }
            if (!ok_dst_ep || dst_ep == 0 || dst_ep > 240) {
                set_err(err, err_size, "bad action.dst_endpoint");
                return ESP_ERR_INVALID_ARG;
            }

            bool ok_cluster = false;
            uint16_t cluster_id = 0;
            if (cbor_map_find_val(a, "cluster_id", &cluster_s)) {
                cluster_id = parse_u16_any_cbor(&cluster_s, &ok_cluster);
            }
            if (!ok_cluster || cluster_id == 0) {
                set_err(err, err_size, "bad action.cluster_id");
                return ESP_ERR_INVALID_ARG;
            }

            acts[i].kind = GW_AUTO_ACT_BIND;
            if (!cbor_text_to_strtab(&src_uid_s, st, &acts[i].uid_off)) {
                set_err(err, err_size, "bad action.src_device_uid");
                return ESP_ERR_INVALID_ARG;
            }
            if (!cbor_text_to_strtab(&dst_uid_s, st, &acts[i].uid2_off)) {
                set_err(err, err_size, "bad action.dst_device_uid");
                return ESP_ERR_INVALID_ARG;
            }
            acts[i].endpoint = (uint8_t)src_ep;
            acts[i].aux_ep = (uint8_t)dst_ep;
            acts[i].u16_0 = cluster_id;
            acts[i].flags = (cbor_text_equals(&cmd_s, "unbind") || cbor_text_equals(&cmd_s, "bindings.unbind"))
                                ? GW_AUTO_ACT_FLAG_UNBIND
                                : 0;
            continue;
        }

        // 2) Scenes (group-based)
        if (cbor_text_equals(&cmd_s, "scene.store") || cbor_text_equals(&cmd_s, "scene.recall")) {
            gw_cbor_slice_t group_s = {0};
            gw_cbor_slice_t scene_s = {0};
            bool ok_gid = false;
            uint16_t group_id = 0;
            if (cbor_map_find_val(a, "group_id", &group_s)) {
                group_id = parse_u16_any_cbor(&group_s, &ok_gid);
            }
            if (!ok_gid || group_id == 0 || group_id == 0xFFFF) {
                set_err(err, err_size, "bad action.group_id");
                return ESP_ERR_INVALID_ARG;
            }

            bool ok_scene = false;
            uint32_t scene_id = 0;
            if (cbor_map_find_val(a, "scene_id", &scene_s)) {
                scene_id = parse_u32_any_cbor(&scene_s, &ok_scene);
            }
            if (!ok_scene || scene_id == 0 || scene_id > 255) {
                set_err(err, err_size, "bad action.scene_id");
                return ESP_ERR_INVALID_ARG;
            }

            acts[i].kind = GW_AUTO_ACT_SCENE;
            acts[i].u16_0 = group_id;
            acts[i].u16_1 = (uint16_t)scene_id;
            continue;
        }

        // 3) Group actions (groupcast) -- detected by presence of group_id
        gw_cbor_slice_t group_s = {0};
        bool ok_gid = false;
        uint16_t group_id = 0;
        if (cbor_map_find_val(a, "group_id", &group_s)) {
            group_id = parse_u16_any_cbor(&group_s, &ok_gid);
        }
        if (ok_gid && group_id != 0 && group_id != 0xFFFF) {
            acts[i].kind = GW_AUTO_ACT_GROUP;
            acts[i].u16_0 = group_id;

            if (cbor_text_equals(&cmd_s, "level.move_to_level")) {
                gw_cbor_slice_t lvl_s = {0};
                gw_cbor_slice_t tr_s = {0};
                bool ok_lvl = false;
                uint32_t lvl = 0;
                if (cbor_map_find_val(a, "level", &lvl_s)) {
                    lvl = parse_u32_any_cbor(&lvl_s, &ok_lvl);
                }
                if (!ok_lvl || lvl > 254) {
                    set_err(err, err_size, "bad action.level");
                    return ESP_ERR_INVALID_ARG;
                }
                bool ok_tr = false;
                uint32_t tr = 0;
                if (cbor_map_find_val(a, "transition_ms", &tr_s)) {
                    tr = parse_u32_any_cbor(&tr_s, &ok_tr);
                }
                acts[i].arg0_u32 = lvl;
                acts[i].arg1_u32 = ok_tr ? tr : 0;
            } else if (cbor_text_equals(&cmd_s, "color.move_to_color_xy")) {
                gw_cbor_slice_t x_s = {0};
                gw_cbor_slice_t y_s = {0};
                gw_cbor_slice_t tr_s = {0};

                bool ok_x = false;
                bool ok_y = false;
                uint32_t x = 0;
                uint32_t y = 0;
                if (cbor_map_find_val(a, "x", &x_s)) {
                    x = parse_u32_any_cbor(&x_s, &ok_x);
                }
                if (cbor_map_find_val(a, "y", &y_s)) {
                    y = parse_u32_any_cbor(&y_s, &ok_y);
                }
                if (!ok_x || x > 65535) {
                    set_err(err, err_size, "bad action.x");
                    return ESP_ERR_INVALID_ARG;
                }
                if (!ok_y || y > 65535) {
                    set_err(err, err_size, "bad action.y");
                    return ESP_ERR_INVALID_ARG;
                }
                bool ok_tr = false;
                uint32_t tr = 0;
                if (cbor_map_find_val(a, "transition_ms", &tr_s)) {
                    tr = parse_u32_any_cbor(&tr_s, &ok_tr);
                }
                acts[i].arg0_u32 = x;
                acts[i].arg1_u32 = y;
                acts[i].arg2_u32 = ok_tr ? tr : 0;
            } else if (cbor_text_equals(&cmd_s, "color.move_to_color_temperature")) {
                gw_cbor_slice_t m_s = {0};
                gw_cbor_slice_t tr_s = {0};

                bool ok_m = false;
                uint32_t mireds = 0;
                if (cbor_map_find_val(a, "mireds", &m_s)) {
                    mireds = parse_u32_any_cbor(&m_s, &ok_m);
                }
                if (!ok_m || mireds < 1 || mireds > 1000) {
                    set_err(err, err_size, "bad action.mireds");
                    return ESP_ERR_INVALID_ARG;
                }
                bool ok_tr = false;
                uint32_t tr = 0;
                if (cbor_map_find_val(a, "transition_ms", &tr_s)) {
                    tr = parse_u32_any_cbor(&tr_s, &ok_tr);
                }
                acts[i].arg0_u32 = mireds;
                acts[i].arg1_u32 = ok_tr ? tr : 0;
            }
            continue;
        }

        // 4) Device actions (unicast)
        gw_cbor_slice_t uid_s = {0};
        gw_cbor_slice_t ep_s = {0};
        if (!cbor_map_find_val(a, "device_uid", &uid_s) || !uid_s.ptr) {
            set_err(err, err_size, "missing action.device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        if (!cbor_text_is_uid(&uid_s)) {
            set_err(err, err_size, "bad action.device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        bool ok_ep = false;
        uint16_t ep = 0;
        if (cbor_map_find_val(a, "endpoint", &ep_s)) {
            ep = parse_u16_any_cbor(&ep_s, &ok_ep);
        }
        if (!ok_ep || ep == 0 || ep > 240) {
            set_err(err, err_size, "bad action.endpoint");
            return ESP_ERR_INVALID_ARG;
        }

        acts[i].kind = GW_AUTO_ACT_DEVICE;
        if (!cbor_text_to_strtab(&uid_s, st, &acts[i].uid_off)) {
            set_err(err, err_size, "bad action.device_uid");
            return ESP_ERR_INVALID_ARG;
        }
        acts[i].endpoint = (uint8_t)ep;

        if (cbor_text_equals(&cmd_s, "level.move_to_level")) {
            gw_cbor_slice_t lvl_s = {0};
            gw_cbor_slice_t tr_s = {0};
            bool ok_lvl = false;
            uint32_t lvl = 0;
            if (cbor_map_find_val(a, "level", &lvl_s)) {
                lvl = parse_u32_any_cbor(&lvl_s, &ok_lvl);
            }
            if (!ok_lvl || lvl > 254) {
                set_err(err, err_size, "bad action.level");
                return ESP_ERR_INVALID_ARG;
            }
            bool ok_tr = false;
            uint32_t tr = 0;
            if (cbor_map_find_val(a, "transition_ms", &tr_s)) {
                tr = parse_u32_any_cbor(&tr_s, &ok_tr);
            }
            acts[i].arg0_u32 = lvl;
            acts[i].arg1_u32 = ok_tr ? tr : 0;
        } else if (cbor_text_equals(&cmd_s, "color.move_to_color_xy")) {
            gw_cbor_slice_t x_s = {0};
            gw_cbor_slice_t y_s = {0};
            gw_cbor_slice_t tr_s = {0};

            bool ok_x = false;
            bool ok_y = false;
            uint32_t x = 0;
            uint32_t y = 0;
            if (cbor_map_find_val(a, "x", &x_s)) {
                x = parse_u32_any_cbor(&x_s, &ok_x);
            }
            if (cbor_map_find_val(a, "y", &y_s)) {
                y = parse_u32_any_cbor(&y_s, &ok_y);
            }
            if (!ok_x || x > 65535) {
                set_err(err, err_size, "bad action.x");
                return ESP_ERR_INVALID_ARG;
            }
            if (!ok_y || y > 65535) {
                set_err(err, err_size, "bad action.y");
                return ESP_ERR_INVALID_ARG;
            }
            bool ok_tr = false;
            uint32_t tr = 0;
            if (cbor_map_find_val(a, "transition_ms", &tr_s)) {
                tr = parse_u32_any_cbor(&tr_s, &ok_tr);
            }
            acts[i].arg0_u32 = x;
            acts[i].arg1_u32 = y;
            acts[i].arg2_u32 = ok_tr ? tr : 0;
        } else if (cbor_text_equals(&cmd_s, "color.move_to_color_temperature")) {
            gw_cbor_slice_t m_s = {0};
            gw_cbor_slice_t tr_s = {0};

            bool ok_m = false;
            uint32_t mireds = 0;
            if (cbor_map_find_val(a, "mireds", &m_s)) {
                mireds = parse_u32_any_cbor(&m_s, &ok_m);
            }
            if (!ok_m || mireds < 1 || mireds > 1000) {
                set_err(err, err_size, "bad action.mireds");
                return ESP_ERR_INVALID_ARG;
            }
            bool ok_tr = false;
            uint32_t tr = 0;
            if (cbor_map_find_val(a, "transition_ms", &tr_s)) {
                tr = parse_u32_any_cbor(&tr_s, &ok_tr);
            }
            acts[i].arg0_u32 = mireds;
            acts[i].arg1_u32 = ok_tr ? tr : 0;
        }
    }

    return ESP_OK;
}

static esp_err_t compile_from_root_cbor(const uint8_t *buf, size_t len, gw_auto_compiled_t *out, char *err, size_t err_size)
{
    if (!buf || len == 0 || !out) return ESP_ERR_INVALID_ARG;

    uint64_t pairs = 0;
    if (!gw_cbor_top_is_map(buf, len, &pairs)) {
        set_err(err, err_size, "root must be map");
        return ESP_ERR_INVALID_ARG;
    }

    strtab_t st = {0};
    esp_err_t rc = strtab_init(&st);
    if (rc != ESP_OK) {
        set_err(err, err_size, "no mem");
        return rc;
    }

    const gw_cbor_slice_t root = { .ptr = buf, .len = len };
    gw_cbor_slice_t id_s = {0};
    gw_cbor_slice_t name_s = {0};
    gw_cbor_slice_t enabled_s = {0};
    gw_cbor_slice_t triggers_s = {0};
    gw_cbor_slice_t conds_s = {0};
    gw_cbor_slice_t actions_s = {0};

    if (!cbor_map_find_val(&root, "id", &id_s) || !cbor_map_find_val(&root, "name", &name_s)) {
        set_err(err, err_size, "missing id/name");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (!cbor_map_find_val(&root, "triggers", &triggers_s)) {
        set_err(err, err_size, "missing triggers");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (!cbor_map_find_val(&root, "actions", &actions_s)) {
        set_err(err, err_size, "missing actions");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }
    (void)cbor_map_find_val(&root, "enabled", &enabled_s);
    (void)cbor_map_find_val(&root, "conditions", &conds_s);

    const uint8_t *id_p = NULL;
    size_t id_n = 0;
    const uint8_t *name_p = NULL;
    size_t name_n = 0;
    if (!cbor_get_text_span(&id_s, &id_p, &id_n) || !id_p || id_n == 0) {
        set_err(err, err_size, "missing id");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (!cbor_get_text_span(&name_s, &name_p, &name_n) || !name_p) {
        set_err(err, err_size, "missing name");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }

    gw_cbor_slice_t *trigger_items = NULL;
    gw_cbor_slice_t *cond_items = NULL;
    gw_cbor_slice_t *action_items = NULL;
    uint32_t trigger_count = 0;
    uint32_t cond_count = 0;
    uint32_t action_count = 0;

    if (!cbor_array_slices(&triggers_s, &trigger_items, &trigger_count)) {
        set_err(err, err_size, "bad triggers");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (conds_s.ptr && !cbor_array_slices(&conds_s, &cond_items, &cond_count)) {
        set_err(err, err_size, "bad conditions");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (!cbor_array_slices(&actions_s, &action_items, &action_count)) {
        set_err(err, err_size, "bad actions");
        rc = ESP_ERR_INVALID_ARG;
        goto done;
    }

    gw_auto_bin_automation_v2_t *auto_rec = (gw_auto_bin_automation_v2_t *)calloc(1, sizeof(*auto_rec));
    gw_auto_bin_trigger_v2_t *trigs = trigger_count ? (gw_auto_bin_trigger_v2_t *)calloc(trigger_count, sizeof(*trigs)) : NULL;
    gw_auto_bin_condition_v2_t *conds = cond_count ? (gw_auto_bin_condition_v2_t *)calloc(cond_count, sizeof(*conds)) : NULL;
    gw_auto_bin_action_v2_t *acts = action_count ? (gw_auto_bin_action_v2_t *)calloc(action_count, sizeof(*acts)) : NULL;
    if (!auto_rec || (trigger_count && !trigs) || (cond_count && !conds) || (action_count && !acts)) {
        set_err(err, err_size, "no mem");
        rc = ESP_ERR_NO_MEM;
        free(auto_rec);
        free(trigs);
        free(conds);
        free(acts);
        goto done;
    }

    auto_rec->id_off = strtab_add_n(&st, id_p, id_n);
    auto_rec->name_off = strtab_add_n(&st, name_p, name_n);
    auto_rec->enabled = 1;
    if (enabled_s.ptr) {
        bool b = true;
        if (gw_cbor_slice_to_bool(&enabled_s, &b)) {
            auto_rec->enabled = b ? 1 : 0;
        }
    }
    // Only single-run mode is supported in current runtime.
    auto_rec->mode = 1;
    auto_rec->triggers_index = 0;
    auto_rec->triggers_count = trigger_count;
    auto_rec->conditions_index = 0;
    auto_rec->conditions_count = cond_count;
    auto_rec->actions_index = 0;
    auto_rec->actions_count = action_count;
    rc = compile_triggers(trigger_items, trigger_count, trigs, &st, err, err_size);
    if (rc != ESP_OK) goto done_alloc;

    rc = compile_conditions(cond_items, cond_count, conds, &st, err, err_size);
    if (rc != ESP_OK) goto done_alloc;

    rc = compile_actions(action_items, action_count, acts, &st, err, err_size);
    if (rc != ESP_OK) goto done_alloc;

    // Populate output (single automation bundle)
    memset(out, 0, sizeof(*out));
    out->hdr.magic = MAGIC_GWAR;
    out->hdr.version = 2;
    out->hdr.automation_count = 1;
    out->hdr.trigger_count_total = trigger_count;
    out->hdr.condition_count_total = cond_count;
    out->hdr.action_count_total = action_count;

    out->autos = auto_rec;
    out->triggers = trigs;
    out->conditions = conds;
    out->actions = acts;
    out->strings = st.buf;
    st.buf = NULL;
    out->hdr.strings_size = (uint32_t)st.len;

    rc = ESP_OK;
    goto done;

done_alloc:
    free(auto_rec);
    free(trigs);
    free(conds);
    free(acts);
done:
    free(trigger_items);
    free(cond_items);
    free(action_items);
    strtab_free(&st);
    return rc;
}

esp_err_t gw_auto_compile_cbor(const uint8_t *buf, size_t len, gw_auto_compiled_t *out, char *err, size_t err_size)
{
    set_err(err, err_size, NULL);
    if (!buf || len == 0 || !out) {
        set_err(err, err_size, "bad args");
        return ESP_ERR_INVALID_ARG;
    }
    return compile_from_root_cbor(buf, len, out, err, err_size);
}
void gw_auto_compiled_free(gw_auto_compiled_t *c)
{
    if (!c) return;
    free(c->autos);
    free(c->triggers);
    free(c->conditions);
    free(c->actions);
    free(c->strings);
    *c = (gw_auto_compiled_t){0};
}

esp_err_t gw_auto_compiled_serialize(const gw_auto_compiled_t *c, uint8_t **out_buf, size_t *out_len)
{
    if (!c || !out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    if (c->hdr.magic != MAGIC_GWAR || c->hdr.version != 2) return ESP_ERR_INVALID_ARG;

    const size_t hdr_sz = sizeof(gw_auto_bin_header_v2_t);
    const size_t autos_sz = (size_t)c->hdr.automation_count * sizeof(gw_auto_bin_automation_v2_t);
    const size_t tr_sz = (size_t)c->hdr.trigger_count_total * sizeof(gw_auto_bin_trigger_v2_t);
    const size_t co_sz = (size_t)c->hdr.condition_count_total * sizeof(gw_auto_bin_condition_v2_t);
    const size_t ac_sz = (size_t)c->hdr.action_count_total * sizeof(gw_auto_bin_action_v2_t);
    const size_t st_sz = (size_t)c->hdr.strings_size;

    gw_auto_bin_header_v2_t hdr = c->hdr;
    hdr.automations_off = (uint32_t)hdr_sz;
    hdr.triggers_off = (uint32_t)(hdr_sz + autos_sz);
    hdr.conditions_off = (uint32_t)(hdr.triggers_off + tr_sz);
    hdr.actions_off = (uint32_t)(hdr.conditions_off + co_sz);
    hdr.strings_off = (uint32_t)(hdr.actions_off + ac_sz);
    hdr.strings_size = (uint32_t)st_sz;

    const size_t total = hdr_sz + autos_sz + tr_sz + co_sz + ac_sz + st_sz;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) return ESP_ERR_NO_MEM;

    // Header: memcpy is OK (same target arch), but keep magic/version explicit to avoid surprises.
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + hdr.automations_off, c->autos, autos_sz);
    memcpy(buf + hdr.triggers_off, c->triggers, tr_sz);
    memcpy(buf + hdr.conditions_off, c->conditions, co_sz);
    memcpy(buf + hdr.actions_off, c->actions, ac_sz);
    memcpy(buf + hdr.strings_off, c->strings, st_sz);

    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

esp_err_t gw_auto_compiled_deserialize(const uint8_t *buf, size_t len, gw_auto_compiled_t *out)
{
    if (!buf || !out || len < sizeof(gw_auto_bin_header_v2_t)) return ESP_ERR_INVALID_ARG;

    gw_auto_bin_header_v2_t hdr = {0};
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != MAGIC_GWAR || hdr.version != 2) return ESP_ERR_INVALID_ARG;

    // Basic bounds checks
    if (hdr.strings_off > len || hdr.strings_size > len || hdr.strings_off + hdr.strings_size > len) return ESP_ERR_INVALID_ARG;

    const size_t autos_sz = (size_t)hdr.automation_count * sizeof(gw_auto_bin_automation_v2_t);
    const size_t tr_sz = (size_t)hdr.trigger_count_total * sizeof(gw_auto_bin_trigger_v2_t);
    const size_t co_sz = (size_t)hdr.condition_count_total * sizeof(gw_auto_bin_condition_v2_t);
    const size_t ac_sz = (size_t)hdr.action_count_total * sizeof(gw_auto_bin_action_v2_t);

    if ((size_t)hdr.automations_off + autos_sz > len) return ESP_ERR_INVALID_ARG;
    if ((size_t)hdr.triggers_off + tr_sz > len) return ESP_ERR_INVALID_ARG;
    if ((size_t)hdr.conditions_off + co_sz > len) return ESP_ERR_INVALID_ARG;
    if ((size_t)hdr.actions_off + ac_sz > len) return ESP_ERR_INVALID_ARG;

    gw_auto_compiled_t c = {0};
    c.hdr = hdr;
    c.autos = hdr.automation_count ? (gw_auto_bin_automation_v2_t *)calloc(hdr.automation_count, sizeof(*c.autos)) : NULL;
    c.triggers = hdr.trigger_count_total ? (gw_auto_bin_trigger_v2_t *)calloc(hdr.trigger_count_total, sizeof(*c.triggers)) : NULL;
    c.conditions = hdr.condition_count_total ? (gw_auto_bin_condition_v2_t *)calloc(hdr.condition_count_total, sizeof(*c.conditions)) : NULL;
    c.actions = hdr.action_count_total ? (gw_auto_bin_action_v2_t *)calloc(hdr.action_count_total, sizeof(*c.actions)) : NULL;
    c.strings = hdr.strings_size ? (char *)calloc(1, hdr.strings_size) : NULL;

    if ((hdr.automation_count && !c.autos) || (hdr.trigger_count_total && !c.triggers) || (hdr.condition_count_total && !c.conditions) ||
        (hdr.action_count_total && !c.actions) || (hdr.strings_size && !c.strings)) {
        gw_auto_compiled_free(&c);
        return ESP_ERR_NO_MEM;
    }

    memcpy(c.autos, buf + hdr.automations_off, autos_sz);
    memcpy(c.triggers, buf + hdr.triggers_off, tr_sz);
    memcpy(c.conditions, buf + hdr.conditions_off, co_sz);
    memcpy(c.actions, buf + hdr.actions_off, ac_sz);
    memcpy(c.strings, buf + hdr.strings_off, hdr.strings_size);

    *out = c;
    return ESP_OK;
}

esp_err_t gw_auto_compiled_write_file(const char *path, const gw_auto_compiled_t *c)
{
    if (!path || !c) return ESP_ERR_INVALID_ARG;
    uint8_t *buf = NULL;
    size_t len = 0;
    esp_err_t err = gw_auto_compiled_serialize(c, &buf, &len);
    if (err != ESP_OK) return err;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return ESP_FAIL;
    }
    size_t w = fwrite(buf, 1, len, f);
    (void)fclose(f);
    free(buf);
    return (w == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_auto_compiled_read_file(const char *path, gw_auto_compiled_t *out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)sz);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) {
        free(buf);
        return ESP_FAIL;
    }
    esp_err_t err = gw_auto_compiled_deserialize(buf, (size_t)sz, out);
    free(buf);
    return err;
}
