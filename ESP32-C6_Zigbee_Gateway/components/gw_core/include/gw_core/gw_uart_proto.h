#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Универсальный и простой UART-протокол для связки C6 <-> S3.
 *
 * Формат кадра (little-endian):
 *   [0]  SOF0 = 0xA5
 *   [1]  SOF1 = 0x5A
 *   [2]  ver
 *   [3]  msg_type
 *   [4]  flags
 *   [5]  seq_l
 *   [6]  seq_h
 *   [7]  payload_len_l
 *   [8]  payload_len_h
 *   [9..] payload bytes
 *   [end-2] crc16_l   \
 *   [end-1] crc16_h   / CRC16-CCITT(FALSЕ) по полям [ver..payload]
 */

#define GW_UART_PROTO_SOF0             0xA5u
#define GW_UART_PROTO_SOF1             0x5Au
#define GW_UART_PROTO_VERSION_V1       1u
#define GW_UART_PROTO_HEADER_SIZE      9u
#define GW_UART_PROTO_CRC_SIZE         2u
#define GW_UART_PROTO_MAX_PAYLOAD      192u
#define GW_UART_PROTO_MAX_FRAME_SIZE   (GW_UART_PROTO_HEADER_SIZE + GW_UART_PROTO_MAX_PAYLOAD + GW_UART_PROTO_CRC_SIZE)

typedef enum {
    GW_UART_MSG_HELLO    = 0x01, /* обмен версиями/ролями при старте */
    GW_UART_MSG_HELLO_ACK= 0x02,
    GW_UART_MSG_PING     = 0x03,
    GW_UART_MSG_PONG     = 0x04,

    GW_UART_MSG_CMD_REQ  = 0x10, /* команда S3 -> C6 */
    GW_UART_MSG_CMD_RSP  = 0x11, /* ответ C6 -> S3 */

    GW_UART_MSG_EVT      = 0x20, /* асинхронное событие C6 -> S3 */
    GW_UART_MSG_SNAPSHOT = 0x21, /* пакет состояния при синхронизации */
    GW_UART_MSG_DEVICE_FB = 0x22, /* сырой device FlatBuffer chunk C6 -> S3 */
} gw_uart_msg_type_t;

typedef enum {
    GW_UART_CMD_ONOFF      = 1, /* param0: 0=off,1=on,2=toggle */
    GW_UART_CMD_LEVEL      = 2, /* param0: level(0..254), param1: transition_ds */
    GW_UART_CMD_COLOR_XY   = 3, /* param0: x(0..65535), param1: y(0..65535), param2: transition_ds */
    GW_UART_CMD_COLOR_TEMP = 4, /* param0: mired, param1: transition_ds */
    GW_UART_CMD_PERMIT_JOIN= 5, /* param0: seconds */
    GW_UART_CMD_READ_ATTR  = 6, /* cluster_id + attr_id */
    GW_UART_CMD_WRITE_ATTR = 7, /* cluster_id + attr_id + value_* */
    GW_UART_CMD_IDENTIFY   = 8, /* param0: seconds */
    GW_UART_CMD_SYNC_SNAPSHOT = 9, /* запрос полного списка устройств/endpoint от C6 */
    GW_UART_CMD_SYNC_DEVICE_FB = 10, /* запрос сырого device FlatBuffer снимка */
    GW_UART_CMD_SET_DEVICE_NAME = 11, /* device_uid + value_text */
    GW_UART_CMD_REMOVE_DEVICE = 12, /* device_uid */
    GW_UART_CMD_WIFI_CONFIG_SET = 13, /* value_blob: ssid\0password\0 */
    GW_UART_CMD_NET_SERVICES_START = 14, /* trigger immediate time/weather sync */
} gw_uart_cmd_id_t;

typedef enum {
    GW_UART_EVT_ATTR_REPORT = 1,
    GW_UART_EVT_COMMAND     = 2,
    GW_UART_EVT_DEVICE_JOIN = 3,
    GW_UART_EVT_DEVICE_LEAVE= 4,
    GW_UART_EVT_NET_STATE   = 5,
} gw_uart_evt_id_t;

typedef enum {
    GW_UART_VALUE_NONE = 0,
    GW_UART_VALUE_BOOL = 1,
    GW_UART_VALUE_I64  = 2,
    GW_UART_VALUE_F32  = 3,
    GW_UART_VALUE_TEXT = 4,
} gw_uart_value_type_t;

typedef enum {
    GW_UART_STATUS_OK                  = 0,
    GW_UART_STATUS_INVALID_ARGS        = 1,
    GW_UART_STATUS_NOT_READY           = 2,
    GW_UART_STATUS_NOT_FOUND           = 3,
    GW_UART_STATUS_UNSUPPORTED         = 4,
    GW_UART_STATUS_BUSY                = 5,
    GW_UART_STATUS_TIMEOUT             = 6,
    GW_UART_STATUS_INTERNAL_ERROR      = 7,
    GW_UART_STATUS_TRANSPORT_CRC_ERROR = 100,
    GW_UART_STATUS_TRANSPORT_FORMAT    = 101,
} gw_uart_status_t;

typedef enum {
    GW_UART_SNAPSHOT_BEGIN    = 1,
    GW_UART_SNAPSHOT_DEVICE   = 2,
    GW_UART_SNAPSHOT_ENDPOINT = 3,
    GW_UART_SNAPSHOT_REMOVE   = 4,
    GW_UART_SNAPSHOT_END      = 5,
    GW_UART_SNAPSHOT_STATE    = 6,
} gw_uart_snapshot_kind_t;

/* Логический кадр после успешного разбора transport-уровня. */
typedef struct {
    uint8_t ver;
    uint8_t msg_type;
    uint8_t flags;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t payload[GW_UART_PROTO_MAX_PAYLOAD];
} gw_uart_proto_frame_t;

#if defined(__GNUC__)
#define GW_UART_PROTO_PACKED __attribute__((packed))
#else
#define GW_UART_PROTO_PACKED
#endif

/* Команда S3 -> C6 (payload для GW_UART_MSG_CMD_REQ). */
typedef struct {
    uint32_t req_id;             /* корреляция на уровне приложения */
    uint8_t cmd_id;              /* gw_uart_cmd_id_t */
    char device_uid[19];         /* "0x00124B0012345678" или "" */
    uint16_t short_addr;         /* 0 если неизвестен */
    uint8_t endpoint;            /* 0 если не используется */
    uint16_t cluster_id;         /* 0 если не используется */
    uint16_t attr_id;            /* 0 если не используется */
    int32_t param0;
    int32_t param1;
    int32_t param2;
    uint8_t value_type;          /* gw_uart_value_type_t */
    uint8_t value_bool;
    int64_t value_i64;
    float value_f32;
    char value_text[24];
    char value_blob[96];
} GW_UART_PROTO_PACKED gw_uart_cmd_req_v1_t;

/* Ответ C6 -> S3 (payload для GW_UART_MSG_CMD_RSP). */
typedef struct {
    uint32_t req_id;
    uint16_t status;             /* gw_uart_status_t */
    uint16_t zb_status;          /* сырой Zigbee status при наличии */
    char message[32];            /* короткий reason/debug */
} GW_UART_PROTO_PACKED gw_uart_cmd_rsp_v1_t;

/* Событие C6 -> S3 (payload для GW_UART_MSG_EVT). */
typedef struct {
    uint32_t event_id;
    uint64_t ts_ms;
    uint8_t evt_id;              /* gw_uart_evt_id_t */
    char event_type[32];         /* строковый тип из event_bus (для совместимости) */
    char cmd[16];                /* payload.cmd для zigbee.command (например "toggle") */
    char device_uid[19];
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t value_type;          /* gw_uart_value_type_t */
    uint8_t value_bool;
    int64_t value_i64;
    float value_f32;
    char value_text[24];
} GW_UART_PROTO_PACKED gw_uart_evt_v1_t;

/* Снимок состояния C6 -> S3 (payload для GW_UART_MSG_SNAPSHOT). */
#define GW_UART_SNAPSHOT_MAX_CLUSTERS 8
typedef struct {
    uint8_t kind;                /* gw_uart_snapshot_kind_t */
    uint8_t flags;               /* зарезервировано */
    uint16_t total_devices;      /* валидно для BEGIN */
    uint32_t snapshot_seq;       /* порядковый номер записи внутри снимка */

    char device_uid[19];
    uint16_t short_addr;
    uint64_t last_seen_ms;
    uint8_t has_onoff;
    uint8_t has_button;
    char name[32];

    uint8_t endpoint;            /* валидно для ENDPOINT */
    uint16_t profile_id;         /* валидно для ENDPOINT */
    uint16_t device_id;          /* валидно для ENDPOINT */
    uint8_t in_cluster_count;
    uint8_t out_cluster_count;
    uint16_t in_clusters[GW_UART_SNAPSHOT_MAX_CLUSTERS];
    uint16_t out_clusters[GW_UART_SNAPSHOT_MAX_CLUSTERS];

    /* valid for STATE */
    uint16_t state_cluster_id;
    uint16_t state_attr_id;
    uint8_t state_value_type;    /* gw_uart_value_type_t */
    uint8_t state_value_bool;
    int64_t state_value_i64;
    float state_value_f32;
    char state_value_text[24];
    uint64_t state_ts_ms;
} GW_UART_PROTO_PACKED gw_uart_snapshot_v1_t;

/* Chunk сырого device buffer (FlatBuffer) C6 -> S3. */
#define GW_UART_DEVICE_FB_FLAG_BEGIN 0x01u
#define GW_UART_DEVICE_FB_FLAG_END   0x02u

typedef struct {
    uint16_t transfer_id;
    uint32_t total_len;
    uint32_t offset;
    uint8_t chunk_len;
    uint8_t flags;               /* GW_UART_DEVICE_FB_FLAG_* */
    uint8_t data[180];
} GW_UART_PROTO_PACKED gw_uart_device_fb_chunk_v1_t;

/*
 * Парсер потокового UART.
 * Идея: хранит внутренний буфер и умеет "доклеивать" куски байт, пока
 * не соберется полный кадр.
 */
typedef struct {
    uint8_t buf[GW_UART_PROTO_MAX_FRAME_SIZE];
    size_t len;
    size_t expected_len;
    uint8_t state;
} gw_uart_proto_parser_t;

uint16_t gw_uart_proto_crc16_ccitt_false(const uint8_t *data, size_t len);

esp_err_t gw_uart_proto_build_frame(const gw_uart_proto_frame_t *frame, uint8_t *out, size_t out_size, size_t *out_len);

void gw_uart_proto_parser_init(gw_uart_proto_parser_t *parser);

/*
 * Кормим парсер чанком байт.
 * out_consumed: сколько байт из data обработано.
 * out_ready=true: получен валидный кадр в out_frame.
 */
esp_err_t gw_uart_proto_parser_feed(gw_uart_proto_parser_t *parser,
                                    const uint8_t *data,
                                    size_t data_len,
                                    gw_uart_proto_frame_t *out_frame,
                                    bool *out_ready,
                                    size_t *out_consumed);

#ifdef __cplusplus
}
#endif
