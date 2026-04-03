#include "gw_proto/gw_proto_frame.h"

#include <string.h>

esp_err_t gw_proto_frame_build(const gw_proto_hdr_t *hdr,
                               const void *payload,
                               uint16_t payload_len,
                               uint8_t *out,
                               size_t out_size,
                               size_t *out_len)
{
    if (!hdr || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len != hdr->len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > GW_PROTO_FRAME_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t total = sizeof(gw_proto_hdr_t) + payload_len;
    if (out_size < total) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(out, hdr, sizeof(gw_proto_hdr_t));
    if (payload_len > 0) {
        if (!payload) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(out + sizeof(gw_proto_hdr_t), payload, payload_len);
    }
    *out_len = total;
    return ESP_OK;
}

esp_err_t gw_proto_frame_parse(const uint8_t *buf,
                               size_t len,
                               gw_proto_hdr_t *out_hdr,
                               const uint8_t **out_payload)
{
    if (!buf || len < sizeof(gw_proto_hdr_t) || !out_hdr || !out_payload) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(out_hdr, buf, sizeof(gw_proto_hdr_t));
    if (out_hdr->version != GW_PROTO_VERSION_V1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_hdr->len > GW_PROTO_FRAME_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len != sizeof(gw_proto_hdr_t) + out_hdr->len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_payload = buf + sizeof(gw_proto_hdr_t);
    return ESP_OK;
}
