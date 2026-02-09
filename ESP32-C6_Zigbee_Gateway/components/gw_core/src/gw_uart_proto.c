#include "gw_core/gw_uart_proto.h"

#include <string.h>

enum {
    PARSER_SYNC0 = 0,
    PARSER_SYNC1 = 1,
    PARSER_BODY  = 2,
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

uint16_t gw_uart_proto_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    /* Полином 0x1021, init=0xFFFF, refin/refout=false, xorout=0x0000 */
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

esp_err_t gw_uart_proto_build_frame(const gw_uart_proto_frame_t *frame, uint8_t *out, size_t out_size, size_t *out_len)
{
    if (!frame || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame->payload_len > GW_UART_PROTO_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t need = GW_UART_PROTO_HEADER_SIZE + (size_t)frame->payload_len + GW_UART_PROTO_CRC_SIZE;
    if (out_size < need) {
        return ESP_ERR_INVALID_SIZE;
    }

    out[0] = GW_UART_PROTO_SOF0;
    out[1] = GW_UART_PROTO_SOF1;
    out[2] = frame->ver;
    out[3] = frame->msg_type;
    out[4] = frame->flags;
    wr_u16_le(&out[5], frame->seq);
    wr_u16_le(&out[7], frame->payload_len);

    if (frame->payload_len > 0) {
        memcpy(&out[GW_UART_PROTO_HEADER_SIZE], frame->payload, frame->payload_len);
    }

    uint16_t crc = gw_uart_proto_crc16_ccitt_false(&out[2], 7u + frame->payload_len);
    wr_u16_le(&out[GW_UART_PROTO_HEADER_SIZE + frame->payload_len], crc);

    *out_len = need;
    return ESP_OK;
}

void gw_uart_proto_parser_init(gw_uart_proto_parser_t *parser)
{
    if (!parser) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
    parser->state = PARSER_SYNC0;
}

static void parser_reset(gw_uart_proto_parser_t *parser)
{
    parser->len = 0;
    parser->expected_len = 0;
    parser->state = PARSER_SYNC0;
}

esp_err_t gw_uart_proto_parser_feed(gw_uart_proto_parser_t *parser,
                                    const uint8_t *data,
                                    size_t data_len,
                                    gw_uart_proto_frame_t *out_frame,
                                    bool *out_ready,
                                    size_t *out_consumed)
{
    if (!parser || !data || !out_frame || !out_ready || !out_consumed) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_ready = false;
    *out_consumed = 0;

    for (size_t i = 0; i < data_len; ++i) {
        uint8_t b = data[i];
        *out_consumed = i + 1;

        if (parser->state == PARSER_SYNC0) {
            if (b == GW_UART_PROTO_SOF0) {
                parser->buf[0] = b;
                parser->len = 1;
                parser->state = PARSER_SYNC1;
            }
            continue;
        }

        if (parser->state == PARSER_SYNC1) {
            if (b == GW_UART_PROTO_SOF1) {
                parser->buf[1] = b;
                parser->len = 2;
                parser->state = PARSER_BODY;
            } else if (b == GW_UART_PROTO_SOF0) {
                parser->buf[0] = b;
                parser->len = 1;
                parser->state = PARSER_SYNC1;
            } else {
                parser_reset(parser);
            }
            continue;
        }

        if (parser->state == PARSER_BODY) {
            if (parser->len >= sizeof(parser->buf)) {
                parser_reset(parser);
                return ESP_ERR_INVALID_SIZE;
            }
            parser->buf[parser->len++] = b;

            /* Как только приняты первые 9 байт заголовка, знаем ожидаемую длину кадра. */
            if (parser->len == GW_UART_PROTO_HEADER_SIZE) {
                uint16_t payload_len = rd_u16_le(&parser->buf[7]);
                if (payload_len > GW_UART_PROTO_MAX_PAYLOAD) {
                    parser_reset(parser);
                    return ESP_ERR_INVALID_SIZE;
                }
                parser->expected_len = GW_UART_PROTO_HEADER_SIZE + (size_t)payload_len + GW_UART_PROTO_CRC_SIZE;
            }

            if (parser->expected_len > 0 && parser->len == parser->expected_len) {
                uint16_t payload_len = rd_u16_le(&parser->buf[7]);
                uint16_t crc_rx = rd_u16_le(&parser->buf[GW_UART_PROTO_HEADER_SIZE + payload_len]);
                uint16_t crc_calc = gw_uart_proto_crc16_ccitt_false(&parser->buf[2], 7u + payload_len);

                if (crc_rx != crc_calc) {
                    parser_reset(parser);
                    return ESP_ERR_INVALID_CRC;
                }

                out_frame->ver = parser->buf[2];
                out_frame->msg_type = parser->buf[3];
                out_frame->flags = parser->buf[4];
                out_frame->seq = rd_u16_le(&parser->buf[5]);
                out_frame->payload_len = payload_len;
                if (payload_len > 0) {
                    memcpy(out_frame->payload, &parser->buf[GW_UART_PROTO_HEADER_SIZE], payload_len);
                }

                parser_reset(parser);
                *out_ready = true;
                return ESP_OK;
            }
        }
    }

    return ESP_OK;
}

