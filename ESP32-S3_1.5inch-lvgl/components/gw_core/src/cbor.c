// cbor.c - small CBOR reader/writer (definite-length only).

#include "gw_core/cbor.h"

#include <stdlib.h>
#include <string.h>

static bool rd_has(const gw_cbor_reader_t *r, size_t n)
{
    return r && (size_t)(r->end - r->p) >= n;
}

static bool rd_peek_u8(const gw_cbor_reader_t *r, uint8_t *out)
{
    if (!r || !out || !rd_has(r, 1)) return false;
    *out = *r->p;
    return true;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4); }

void gw_cbor_reader_init(gw_cbor_reader_t *r, const uint8_t *buf, size_t len)
{
    if (!r) return;
    r->p = buf;
    r->end = buf ? (buf + len) : NULL;
}

bool gw_cbor_read_u8(gw_cbor_reader_t *r, uint8_t *out)
{
    if (!r || !out || !rd_has(r, 1)) return false;
    *out = *r->p++;
    return true;
}

bool gw_cbor_read_uint_arg(gw_cbor_reader_t *r, uint8_t ai, uint64_t *out)
{
    if (!r || !out) return false;
    if (ai == 31) {
        // Indefinite-length marker. Caller must handle it explicitly.
        return false;
    }
    if (ai < 24) {
        *out = ai;
        return true;
    }
    if (ai == 24) {
        uint8_t v = 0;
        if (!gw_cbor_read_u8(r, &v)) return false;
        *out = v;
        return true;
    }
    if (ai == 25) {
        if (!rd_has(r, 2)) return false;
        *out = be16(r->p);
        r->p += 2;
        return true;
    }
    if (ai == 26) {
        if (!rd_has(r, 4)) return false;
        *out = be32(r->p);
        r->p += 4;
        return true;
    }
    if (ai == 27) {
        if (!rd_has(r, 8)) return false;
        *out = be64(r->p);
        r->p += 8;
        return true;
    }
    return false;
}

bool gw_cbor_read_text_span(gw_cbor_reader_t *r, uint8_t ai, const uint8_t **out_ptr, size_t *out_len)
{
    if (!r || !out_ptr || !out_len) return false;
    uint64_t n = 0;
    if (!gw_cbor_read_uint_arg(r, ai, &n)) return false;
    if (!rd_has(r, (size_t)n)) return false;
    *out_ptr = r->p;
    *out_len = (size_t)n;
    r->p += (size_t)n;
    return true;
}

bool gw_cbor_skip_item(gw_cbor_reader_t *r)
{
    uint8_t ib = 0;
    if (!gw_cbor_read_u8(r, &ib)) return false;
    const uint8_t major = (uint8_t)(ib >> 5);
    const uint8_t ai = (uint8_t)(ib & 0x1f);

    if (major == 0 || major == 1) {
        uint64_t tmp = 0;
        return gw_cbor_read_uint_arg(r, ai, &tmp);
    }
    if (major == 2 || major == 3) {
        if (ai == 31) {
            // Indefinite bytes/text: sequence of definite chunks terminated by break(0xff).
            for (;;) {
                uint8_t pb = 0;
                if (!rd_peek_u8(r, &pb)) return false;
                if (pb == 0xff) {
                    r->p++; // consume break
                    return true;
                }
                uint8_t cib = 0;
                if (!gw_cbor_read_u8(r, &cib)) return false;
                const uint8_t cmajor = (uint8_t)(cib >> 5);
                const uint8_t cai = (uint8_t)(cib & 0x1f);
                if (cmajor != major || cai == 31) return false;
                uint64_t n = 0;
                if (!gw_cbor_read_uint_arg(r, cai, &n)) return false;
                if (!rd_has(r, (size_t)n)) return false;
                r->p += (size_t)n;
            }
        }
        uint64_t n = 0;
        if (!gw_cbor_read_uint_arg(r, ai, &n)) return false;
        if (!rd_has(r, (size_t)n)) return false;
        r->p += (size_t)n;
        return true;
    }
    if (major == 4) {
        if (ai == 31) {
            for (;;) {
                uint8_t pb = 0;
                if (!rd_peek_u8(r, &pb)) return false;
                if (pb == 0xff) {
                    r->p++; // consume break
                    return true;
                }
                if (!gw_cbor_skip_item(r)) return false;
            }
        }
        uint64_t n = 0;
        if (!gw_cbor_read_uint_arg(r, ai, &n)) return false;
        for (uint64_t i = 0; i < n; i++) {
            if (!gw_cbor_skip_item(r)) return false;
        }
        return true;
    }
    if (major == 5) {
        if (ai == 31) {
            for (;;) {
                uint8_t pb = 0;
                if (!rd_peek_u8(r, &pb)) return false;
                if (pb == 0xff) {
                    r->p++; // consume break
                    return true;
                }
                if (!gw_cbor_skip_item(r)) return false; // key
                if (!gw_cbor_skip_item(r)) return false; // value
            }
        }
        uint64_t n = 0;
        if (!gw_cbor_read_uint_arg(r, ai, &n)) return false;
        for (uint64_t i = 0; i < n * 2; i++) {
            if (!gw_cbor_skip_item(r)) return false;
        }
        return true;
    }
    if (major == 7) {
        if (ai == 20 || ai == 21 || ai == 22) return true; // false/true/null
        if (ai == 27) {
            if (!rd_has(r, 8)) return false;
            r->p += 8;
            return true;
        }
        return false;
    }
    return false;
}

bool gw_cbor_top_is_map(const uint8_t *buf, size_t len, uint64_t *out_pairs)
{
    if (!buf || len == 0) return false;
    gw_cbor_reader_t r = {0};
    gw_cbor_reader_init(&r, buf, len);
    uint8_t ib = 0;
    if (!gw_cbor_read_u8(&r, &ib)) return false;
    if ((ib >> 5) != 5) return false;
    const uint8_t ai = (uint8_t)(ib & 0x1f);
    uint64_t pairs = 0;
    if (ai == 31) {
        pairs = UINT64_MAX; // indefinite map
    } else if (!gw_cbor_read_uint_arg(&r, ai, &pairs)) {
        return false;
    }
    if (out_pairs) *out_pairs = pairs;
    return true;
}

static bool slice_from_reader(const uint8_t *start, const gw_cbor_reader_t *r, gw_cbor_slice_t *out)
{
    if (!out || !start || !r) return false;
    out->ptr = start;
    out->len = (size_t)(r->p - start);
    return true;
}

bool gw_cbor_map_find(const uint8_t *buf, size_t len, const char *key, gw_cbor_slice_t *out_val)
{
    if (!buf || len == 0 || !key || !out_val) return false;
    out_val->ptr = NULL;
    out_val->len = 0;

    gw_cbor_reader_t r = {0};
    gw_cbor_reader_init(&r, buf, len);

    uint8_t ib = 0;
    if (!gw_cbor_read_u8(&r, &ib)) return false;
    if ((ib >> 5) != 5) return false;

    const uint8_t ai = (uint8_t)(ib & 0x1f);
    const bool indefinite = (ai == 31);
    uint64_t pairs = 0;
    if (!indefinite && !gw_cbor_read_uint_arg(&r, ai, &pairs)) return false;

    const size_t key_len = strlen(key);
    for (uint64_t i = 0; indefinite || i < pairs; i++) {
        if (indefinite) {
            uint8_t pb = 0;
            if (!rd_peek_u8(&r, &pb)) return false;
            if (pb == 0xff) {
                r.p++; // consume break
                return false;
            }
        }
        // key
        uint8_t kb = 0;
        if (!gw_cbor_read_u8(&r, &kb)) return false;
        if ((kb >> 5) != 3) return false;
        const uint8_t *kptr = NULL;
        size_t klen = 0;
        if (!gw_cbor_read_text_span(&r, (uint8_t)(kb & 0x1f), &kptr, &klen)) return false;

        const uint8_t *vstart = r.p;
        if (!gw_cbor_skip_item(&r)) return false;

        if (klen == key_len && memcmp(kptr, key, key_len) == 0) {
            return slice_from_reader(vstart, &r, out_val);
        }
    }
    return false;
}

bool gw_cbor_slice_to_u64(const gw_cbor_slice_t *s, uint64_t *out)
{
    if (!s || !s->ptr || s->len == 0 || !out) return false;
    gw_cbor_reader_t r = {0};
    gw_cbor_reader_init(&r, s->ptr, s->len);
    uint8_t ib = 0;
    if (!gw_cbor_read_u8(&r, &ib)) return false;
    const uint8_t major = (uint8_t)(ib >> 5);
    const uint8_t ai = (uint8_t)(ib & 0x1f);
    if (major != 0) return false;
    uint64_t v = 0;
    if (!gw_cbor_read_uint_arg(&r, ai, &v)) return false;
    if (r.p != r.end) return false;
    *out = v;
    return true;
}

bool gw_cbor_slice_to_i64(const gw_cbor_slice_t *s, int64_t *out)
{
    if (!s || !s->ptr || s->len == 0 || !out) return false;
    gw_cbor_reader_t r = {0};
    gw_cbor_reader_init(&r, s->ptr, s->len);
    uint8_t ib = 0;
    if (!gw_cbor_read_u8(&r, &ib)) return false;
    const uint8_t major = (uint8_t)(ib >> 5);
    const uint8_t ai = (uint8_t)(ib & 0x1f);
    uint64_t v = 0;
    if (major == 0) {
        if (!gw_cbor_read_uint_arg(&r, ai, &v)) return false;
        if (r.p != r.end) return false;
        *out = (int64_t)v;
        return true;
    }
    if (major == 1) {
        if (!gw_cbor_read_uint_arg(&r, ai, &v)) return false;
        if (r.p != r.end) return false;
        *out = -(int64_t)(v + 1);
        return true;
    }
    return false;
}

bool gw_cbor_slice_to_f64(const gw_cbor_slice_t *s, double *out)
{
    if (!s || !s->ptr || s->len == 0 || !out) return false;
    gw_cbor_reader_t r = {0};
    gw_cbor_reader_init(&r, s->ptr, s->len);
    uint8_t ib = 0;
    if (!gw_cbor_read_u8(&r, &ib)) return false;
    const uint8_t major = (uint8_t)(ib >> 5);
    const uint8_t ai = (uint8_t)(ib & 0x1f);
    if (major == 7 && ai == 27) {
        if (!rd_has(&r, 8)) return false;
        uint64_t u = be64(r.p);
        r.p += 8;
        if (r.p != r.end) return false;
        double d = 0.0;
        memcpy(&d, &u, sizeof(d));
        *out = d;
        return true;
    }
    // Also accept integers.
    int64_t iv = 0;
    if (gw_cbor_slice_to_i64(s, &iv)) {
        *out = (double)iv;
        return true;
    }
    return false;
}

bool gw_cbor_slice_to_bool(const gw_cbor_slice_t *s, bool *out)
{
    if (!s || !s->ptr || s->len == 0 || !out) return false;
    if (s->len != 1) return false;
    const uint8_t b = s->ptr[0];
    if (b == (uint8_t)((7 << 5) | 20)) {
        *out = false;
        return true;
    }
    if (b == (uint8_t)((7 << 5) | 21)) {
        *out = true;
        return true;
    }
    return false;
}

bool gw_cbor_slice_to_text_span(const gw_cbor_slice_t *s, const uint8_t **out_ptr, size_t *out_len)
{
    if (!s || !s->ptr || s->len == 0 || !out_ptr || !out_len) return false;
    gw_cbor_reader_t r = {0};
    gw_cbor_reader_init(&r, s->ptr, s->len);
    uint8_t ib = 0;
    if (!gw_cbor_read_u8(&r, &ib)) return false;
    if ((ib >> 5) != 3) return false;
    const uint8_t *p = NULL;
    size_t n = 0;
    if (!gw_cbor_read_text_span(&r, (uint8_t)(ib & 0x1f), &p, &n)) return false;
    if (r.p != r.end) return false;
    *out_ptr = p;
    *out_len = n;
    return true;
}

// ---- writer ----

static esp_err_t wr_reserve(gw_cbor_writer_t *w, size_t add)
{
    if (!w) return ESP_ERR_INVALID_ARG;
    if (w->len + add <= w->cap) return ESP_OK;
    size_t new_cap = w->cap ? w->cap : 256;
    while (new_cap < w->len + add) new_cap *= 2;
    uint8_t *nb = (uint8_t *)realloc(w->buf, new_cap);
    if (!nb) return ESP_ERR_NO_MEM;
    w->buf = nb;
    w->cap = new_cap;
    return ESP_OK;
}

static esp_err_t wr_u8(gw_cbor_writer_t *w, uint8_t v)
{
    esp_err_t err = wr_reserve(w, 1);
    if (err != ESP_OK) return err;
    w->buf[w->len++] = v;
    return ESP_OK;
}

static esp_err_t wr_mem(gw_cbor_writer_t *w, const void *src, size_t n)
{
    esp_err_t err = wr_reserve(w, n);
    if (err != ESP_OK) return err;
    memcpy(w->buf + w->len, src, n);
    w->len += n;
    return ESP_OK;
}

static esp_err_t wr_uint(gw_cbor_writer_t *w, uint8_t major, uint64_t v)
{
    if (v < 24) {
        return wr_u8(w, (uint8_t)((major << 5) | (uint8_t)v));
    }
    if (v <= 0xff) {
        esp_err_t err = wr_u8(w, (uint8_t)((major << 5) | 24));
        if (err != ESP_OK) return err;
        return wr_u8(w, (uint8_t)v);
    }
    if (v <= 0xffff) {
        uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
        esp_err_t err = wr_u8(w, (uint8_t)((major << 5) | 25));
        if (err != ESP_OK) return err;
        return wr_mem(w, b, sizeof(b));
    }
    if (v <= 0xffffffffULL) {
        uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        esp_err_t err = wr_u8(w, (uint8_t)((major << 5) | 26));
        if (err != ESP_OK) return err;
        return wr_mem(w, b, sizeof(b));
    }
    uint8_t b[8] = {
        (uint8_t)(v >> 56), (uint8_t)(v >> 48), (uint8_t)(v >> 40), (uint8_t)(v >> 32),
        (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),  (uint8_t)v};
    esp_err_t err = wr_u8(w, (uint8_t)((major << 5) | 27));
    if (err != ESP_OK) return err;
    return wr_mem(w, b, sizeof(b));
}

void gw_cbor_writer_init(gw_cbor_writer_t *w)
{
    if (!w) return;
    *w = (gw_cbor_writer_t){0};
}

void gw_cbor_writer_free(gw_cbor_writer_t *w)
{
    if (!w) return;
    free(w->buf);
    *w = (gw_cbor_writer_t){0};
}

esp_err_t gw_cbor_writer_map(gw_cbor_writer_t *w, uint64_t pairs) { return wr_uint(w, 5, pairs); }
esp_err_t gw_cbor_writer_array(gw_cbor_writer_t *w, uint64_t items) { return wr_uint(w, 4, items); }

esp_err_t gw_cbor_writer_text(gw_cbor_writer_t *w, const char *s)
{
    if (!s) s = "";
    const size_t n = strlen(s);
    esp_err_t err = wr_uint(w, 3, (uint64_t)n);
    if (err != ESP_OK) return err;
    return wr_mem(w, s, n);
}

esp_err_t gw_cbor_writer_text_n(gw_cbor_writer_t *w, const uint8_t *s, size_t n)
{
    esp_err_t err = wr_uint(w, 3, (uint64_t)n);
    if (err != ESP_OK) return err;
    return wr_mem(w, s, n);
}

esp_err_t gw_cbor_writer_bytes(gw_cbor_writer_t *w, const uint8_t *s, size_t n)
{
    if (!s && n) return ESP_ERR_INVALID_ARG;
    esp_err_t err = wr_uint(w, 2, (uint64_t)n);
    if (err != ESP_OK) return err;
    return wr_mem(w, s, n);
}

esp_err_t gw_cbor_writer_u64(gw_cbor_writer_t *w, uint64_t v) { return wr_uint(w, 0, v); }

esp_err_t gw_cbor_writer_i64(gw_cbor_writer_t *w, int64_t v)
{
    if (v >= 0) return wr_uint(w, 0, (uint64_t)v);
    uint64_t n = (uint64_t)(-(v + 1));
    return wr_uint(w, 1, n);
}

esp_err_t gw_cbor_writer_f64(gw_cbor_writer_t *w, double v)
{
    // float64: major 7, additional 27
    esp_err_t err = wr_u8(w, (uint8_t)((7 << 5) | 27));
    if (err != ESP_OK) return err;
    uint64_t u = 0;
    memcpy(&u, &v, sizeof(u));
    uint8_t b[8] = {
        (uint8_t)(u >> 56), (uint8_t)(u >> 48), (uint8_t)(u >> 40), (uint8_t)(u >> 32),
        (uint8_t)(u >> 24), (uint8_t)(u >> 16), (uint8_t)(u >> 8),  (uint8_t)u};
    return wr_mem(w, b, sizeof(b));
}

esp_err_t gw_cbor_writer_bool(gw_cbor_writer_t *w, bool v)
{
    return wr_u8(w, (uint8_t)((7 << 5) | (v ? 21 : 20)));
}

esp_err_t gw_cbor_writer_null(gw_cbor_writer_t *w)
{
    return wr_u8(w, (uint8_t)((7 << 5) | 22));
}
