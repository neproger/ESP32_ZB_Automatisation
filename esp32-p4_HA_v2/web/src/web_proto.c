#include "web/web_proto.h"

#include <string.h>

size_t web_frame_encode(uint8_t *out, size_t out_size, uint8_t type, uint16_t seq,
                        const void *payload, uint16_t len)
{
    if (out == NULL || out_size < (size_t)WEB_PROTO_HDR_SIZE + len) {
        return 0;
    }
    out[0] = WEB_PROTO_VER;
    out[1] = type;
    out[2] = (uint8_t)(len & 0xffu);
    out[3] = (uint8_t)(len >> 8);
    out[4] = (uint8_t)(seq & 0xffu);
    out[5] = (uint8_t)(seq >> 8);
    out[6] = 0;
    out[7] = 0;
    if (len > 0 && payload != NULL) {
        memcpy(out + WEB_PROTO_HDR_SIZE, payload, len);
    }
    return (size_t)WEB_PROTO_HDR_SIZE + len;
}

bool web_frame_decode(const uint8_t *buf, size_t len, web_hdr_t *out_hdr, const uint8_t **out_payload,
                      size_t *out_payload_len)
{
    if (buf == NULL || len < WEB_PROTO_HDR_SIZE) {
        return false;
    }
    const uint16_t payload_len = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    if (buf[0] != WEB_PROTO_VER || (size_t)WEB_PROTO_HDR_SIZE + payload_len > len) {
        return false;
    }
    if (out_hdr != NULL) {
        out_hdr->ver = buf[0];
        out_hdr->type = buf[1];
        out_hdr->len = payload_len;
        out_hdr->seq = (uint16_t)(buf[4] | ((uint16_t)buf[5] << 8));
        out_hdr->flags = (uint16_t)(buf[6] | ((uint16_t)buf[7] << 8));
    }
    if (out_payload != NULL) {
        *out_payload = buf + WEB_PROTO_HDR_SIZE;
    }
    if (out_payload_len != NULL) {
        *out_payload_len = payload_len;
    }
    return true;
}
