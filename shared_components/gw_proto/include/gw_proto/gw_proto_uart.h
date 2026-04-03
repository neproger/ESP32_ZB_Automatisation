#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_proto/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_PROTO_UART_SOF0           0xA5u
#define GW_PROTO_UART_SOF1           0x5Au
#define GW_PROTO_UART_CRC_SIZE       2u
#define GW_PROTO_UART_MAX_PAYLOAD    192u
#define GW_PROTO_UART_HEADER_SIZE    ((uint16_t)sizeof(gw_proto_hdr_t))
#define GW_PROTO_UART_MAX_FRAME_SIZE (2u + GW_PROTO_UART_HEADER_SIZE + GW_PROTO_UART_MAX_PAYLOAD + GW_PROTO_UART_CRC_SIZE)

typedef struct {
    gw_proto_hdr_t hdr;
    uint8_t payload[GW_PROTO_UART_MAX_PAYLOAD];
} gw_proto_uart_frame_t;

typedef struct {
    uint8_t buf[GW_PROTO_UART_MAX_FRAME_SIZE];
    size_t len;
    size_t expected_len;
    uint8_t state;
} gw_proto_uart_parser_t;

uint16_t gw_proto_uart_crc16_ccitt_false(const uint8_t *data, size_t len);
esp_err_t gw_proto_uart_build_frame(const gw_proto_hdr_t *hdr,
                                    const void *payload,
                                    uint16_t payload_len,
                                    uint8_t *out,
                                    size_t out_size,
                                    size_t *out_len);
void gw_proto_uart_parser_init(gw_proto_uart_parser_t *parser);
esp_err_t gw_proto_uart_parser_feed(gw_proto_uart_parser_t *parser,
                                    const uint8_t *data,
                                    size_t data_len,
                                    gw_proto_uart_frame_t *out_frame,
                                    bool *out_ready,
                                    size_t *out_consumed);

#ifdef __cplusplus
}
#endif
