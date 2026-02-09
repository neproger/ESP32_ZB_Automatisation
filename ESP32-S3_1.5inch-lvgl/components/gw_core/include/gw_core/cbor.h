// cbor.h - small CBOR reader/writer (definite-length only).
// This module is meant to replace JSON usage on the ESP32.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *ptr;
    size_t len;
} gw_cbor_slice_t;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} gw_cbor_reader_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} gw_cbor_writer_t;

// Reader
void gw_cbor_reader_init(gw_cbor_reader_t *r, const uint8_t *buf, size_t len);
bool gw_cbor_read_u8(gw_cbor_reader_t *r, uint8_t *out);
bool gw_cbor_read_uint_arg(gw_cbor_reader_t *r, uint8_t ai, uint64_t *out);
bool gw_cbor_read_text_span(gw_cbor_reader_t *r, uint8_t ai, const uint8_t **out_ptr, size_t *out_len);
bool gw_cbor_skip_item(gw_cbor_reader_t *r);

// Top-level helpers (no allocations)
bool gw_cbor_top_is_map(const uint8_t *buf, size_t len, uint64_t *out_pairs);
bool gw_cbor_map_find(const uint8_t *buf, size_t len, const char *key, gw_cbor_slice_t *out_val);
bool gw_cbor_slice_to_u64(const gw_cbor_slice_t *s, uint64_t *out);
bool gw_cbor_slice_to_i64(const gw_cbor_slice_t *s, int64_t *out);
bool gw_cbor_slice_to_f64(const gw_cbor_slice_t *s, double *out);
bool gw_cbor_slice_to_bool(const gw_cbor_slice_t *s, bool *out);
bool gw_cbor_slice_to_text_span(const gw_cbor_slice_t *s, const uint8_t **out_ptr, size_t *out_len);

// Writer
void gw_cbor_writer_init(gw_cbor_writer_t *w);
void gw_cbor_writer_free(gw_cbor_writer_t *w);
esp_err_t gw_cbor_writer_map(gw_cbor_writer_t *w, uint64_t pairs);
esp_err_t gw_cbor_writer_array(gw_cbor_writer_t *w, uint64_t items);
esp_err_t gw_cbor_writer_text(gw_cbor_writer_t *w, const char *s);
esp_err_t gw_cbor_writer_text_n(gw_cbor_writer_t *w, const uint8_t *s, size_t n);
esp_err_t gw_cbor_writer_bytes(gw_cbor_writer_t *w, const uint8_t *s, size_t n);
esp_err_t gw_cbor_writer_u64(gw_cbor_writer_t *w, uint64_t v);
esp_err_t gw_cbor_writer_i64(gw_cbor_writer_t *w, int64_t v);
esp_err_t gw_cbor_writer_f64(gw_cbor_writer_t *w, double v);
esp_err_t gw_cbor_writer_bool(gw_cbor_writer_t *w, bool v);
esp_err_t gw_cbor_writer_null(gw_cbor_writer_t *w);

#ifdef __cplusplus
}
#endif
