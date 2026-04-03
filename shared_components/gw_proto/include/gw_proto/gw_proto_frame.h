#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_proto/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_PROTO_FRAME_HEADER_SIZE ((uint16_t)sizeof(gw_proto_hdr_t))
#define GW_PROTO_FRAME_MAX_PAYLOAD 2048u
#define GW_PROTO_FRAME_MAX_SIZE    (GW_PROTO_FRAME_HEADER_SIZE + GW_PROTO_FRAME_MAX_PAYLOAD)

esp_err_t gw_proto_frame_build(const gw_proto_hdr_t *hdr,
                               const void *payload,
                               uint16_t payload_len,
                               uint8_t *out,
                               size_t out_size,
                               size_t *out_len);

esp_err_t gw_proto_frame_parse(const uint8_t *buf,
                               size_t len,
                               gw_proto_hdr_t *out_hdr,
                               const uint8_t **out_payload);

#ifdef __cplusplus
}
#endif
