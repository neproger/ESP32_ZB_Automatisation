#include "gw_proto/gw_proto_uart.h"

#include <string.h>

#if defined(__cplusplus)
static_assert(sizeof(gw_proto_sync_begin_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "sync_begin payload too large");
static_assert(sizeof(gw_proto_sync_end_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "sync_end payload too large");
static_assert(sizeof(gw_proto_device_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "device payload too large");
static_assert(sizeof(gw_proto_device_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "device_remove payload too large");
static_assert(sizeof(gw_proto_endpoint_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "endpoint payload too large");
static_assert(sizeof(gw_proto_endpoint_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "endpoint_remove payload too large");
static_assert(sizeof(gw_proto_state_item_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "state_item payload too large");
static_assert(sizeof(gw_proto_state_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "state_remove payload too large");
static_assert(sizeof(gw_proto_group_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "group payload too large");
static_assert(sizeof(gw_proto_group_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "group_remove payload too large");
static_assert(sizeof(gw_proto_group_item_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "group_item payload too large");
static_assert(sizeof(gw_proto_group_item_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "group_item_remove payload too large");
static_assert(sizeof(gw_proto_settings_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "settings payload too large");
#else
_Static_assert(sizeof(gw_proto_sync_begin_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "sync_begin payload too large");
_Static_assert(sizeof(gw_proto_sync_end_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "sync_end payload too large");
_Static_assert(sizeof(gw_proto_device_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "device payload too large");
_Static_assert(sizeof(gw_proto_device_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "device_remove payload too large");
_Static_assert(sizeof(gw_proto_endpoint_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "endpoint payload too large");
_Static_assert(sizeof(gw_proto_endpoint_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "endpoint_remove payload too large");
_Static_assert(sizeof(gw_proto_state_item_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "state_item payload too large");
_Static_assert(sizeof(gw_proto_state_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "state_remove payload too large");
_Static_assert(sizeof(gw_proto_group_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "group payload too large");
_Static_assert(sizeof(gw_proto_group_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "group_remove payload too large");
_Static_assert(sizeof(gw_proto_group_item_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "group_item payload too large");
_Static_assert(sizeof(gw_proto_group_item_remove_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "group_item_remove payload too large");
_Static_assert(sizeof(gw_proto_settings_v1_t) <= GW_PROTO_UART_MAX_PAYLOAD, "settings payload too large");
#endif

enum {
    PARSER_SYNC0 = 0,
    PARSER_SYNC1 = 1,
    PARSER_BODY = 2,
};

static uint16_t rd_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void wr_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

uint16_t gw_proto_uart_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    if (!data || len == 0) {
        return crc;
    }
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

esp_err_t gw_proto_uart_build_frame(const gw_proto_hdr_t *hdr,
                                    const void *payload,
                                    uint16_t payload_len,
                                    uint8_t *out,
                                    size_t out_size,
                                    size_t *out_len)
{
    if (!hdr || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > GW_PROTO_UART_MAX_PAYLOAD || hdr->len != payload_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t need = 2u + sizeof(gw_proto_hdr_t) + (size_t)payload_len + GW_PROTO_UART_CRC_SIZE;
    if (out_size < need) {
        return ESP_ERR_INVALID_SIZE;
    }

    out[0] = GW_PROTO_UART_SOF0;
    out[1] = GW_PROTO_UART_SOF1;
    out[2] = hdr->version;
    out[3] = hdr->type;
    wr_u16_le(&out[4], hdr->len);
    wr_u16_le(&out[6], hdr->seq);
    wr_u16_le(&out[8], hdr->reserved);

    if (payload_len > 0 && payload) {
        memcpy(&out[2 + sizeof(gw_proto_hdr_t)], payload, payload_len);
    }

    const uint16_t crc = gw_proto_uart_crc16_ccitt_false(&out[2], sizeof(gw_proto_hdr_t) + payload_len);
    wr_u16_le(&out[2 + sizeof(gw_proto_hdr_t) + payload_len], crc);

    *out_len = need;
    return ESP_OK;
}

void gw_proto_uart_parser_init(gw_proto_uart_parser_t *parser)
{
    if (!parser) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
    parser->state = PARSER_SYNC0;
}

static void parser_reset(gw_proto_uart_parser_t *parser)
{
    parser->len = 0;
    parser->expected_len = 0;
    parser->state = PARSER_SYNC0;
}

esp_err_t gw_proto_uart_parser_feed(gw_proto_uart_parser_t *parser,
                                    const uint8_t *data,
                                    size_t data_len,
                                    gw_proto_uart_frame_t *out_frame,
                                    bool *out_ready,
                                    size_t *out_consumed)
{
    if (!parser || !data || !out_frame || !out_ready || !out_consumed) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_ready = false;
    *out_consumed = 0;

    for (size_t i = 0; i < data_len; ++i) {
        const uint8_t b = data[i];
        *out_consumed = i + 1;

        if (parser->state == PARSER_SYNC0) {
            if (b == GW_PROTO_UART_SOF0) {
                parser->buf[0] = b;
                parser->len = 1;
                parser->state = PARSER_SYNC1;
            }
            continue;
        }

        if (parser->state == PARSER_SYNC1) {
            if (b == GW_PROTO_UART_SOF1) {
                parser->buf[1] = b;
                parser->len = 2;
                parser->state = PARSER_BODY;
            } else if (b == GW_PROTO_UART_SOF0) {
                parser->buf[0] = b;
                parser->len = 1;
                parser->state = PARSER_SYNC1;
            } else {
                parser_reset(parser);
            }
            continue;
        }

        if (parser->len >= sizeof(parser->buf)) {
            parser_reset(parser);
            return ESP_ERR_INVALID_SIZE;
        }
        parser->buf[parser->len++] = b;

        if (parser->len == (2u + sizeof(gw_proto_hdr_t))) {
            const uint16_t payload_len = rd_u16_le(&parser->buf[4]);
            if (payload_len > GW_PROTO_UART_MAX_PAYLOAD) {
                parser_reset(parser);
                return ESP_ERR_INVALID_SIZE;
            }
            parser->expected_len = 2u + sizeof(gw_proto_hdr_t) + (size_t)payload_len + GW_PROTO_UART_CRC_SIZE;
        }

        if (parser->expected_len > 0 && parser->len == parser->expected_len) {
            const uint16_t payload_len = rd_u16_le(&parser->buf[4]);
            const uint16_t crc_rx = rd_u16_le(&parser->buf[2 + sizeof(gw_proto_hdr_t) + payload_len]);
            const uint16_t crc_calc = gw_proto_uart_crc16_ccitt_false(&parser->buf[2], sizeof(gw_proto_hdr_t) + payload_len);
            if (crc_rx != crc_calc) {
                parser_reset(parser);
                return ESP_ERR_INVALID_CRC;
            }

            out_frame->hdr.version = parser->buf[2];
            out_frame->hdr.type = parser->buf[3];
            out_frame->hdr.len = payload_len;
            out_frame->hdr.seq = rd_u16_le(&parser->buf[6]);
            out_frame->hdr.reserved = rd_u16_le(&parser->buf[8]);
            if (payload_len > 0) {
                memcpy(out_frame->payload, &parser->buf[2 + sizeof(gw_proto_hdr_t)], payload_len);
            }

            parser_reset(parser);
            *out_ready = true;
            return ESP_OK;
        }
    }

    return ESP_OK;
}
