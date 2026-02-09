#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "gw_core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(GW_EVENT_BASE);

typedef enum {
    GW_EVENT_SYSTEM_BOOT = 1,
    GW_EVENT_API_REQUEST = 100,
    GW_EVENT_API_RESPONSE,
    GW_EVENT_ZIGBEE_RAW = 200,
    GW_EVENT_ZIGBEE_NORMALIZED,
    GW_EVENT_RULE_ACTION = 300,
    GW_EVENT_RULE_RESULT,
} gw_event_id_t;

typedef enum {
    GW_EVENT_PAYLOAD_HAS_ENDPOINT = 1 << 0,
    GW_EVENT_PAYLOAD_HAS_CMD      = 1 << 1,
    GW_EVENT_PAYLOAD_HAS_CLUSTER  = 1 << 2,
    GW_EVENT_PAYLOAD_HAS_ATTR     = 1 << 3,
    GW_EVENT_PAYLOAD_HAS_VALUE    = 1 << 4,
} gw_event_payload_flag_t;

typedef enum {
    GW_EVENT_VALUE_NONE = 0,
    GW_EVENT_VALUE_BOOL = 1,
    GW_EVENT_VALUE_I64  = 2,
    GW_EVENT_VALUE_F64  = 3,
    GW_EVENT_VALUE_TEXT = 4,
} gw_event_value_type_t;

typedef struct {
    uint8_t v; // event schema version (for clients)
    uint32_t id;
    uint64_t ts_ms;
    char type[32];
    char source[16];
    char device_uid[GW_DEVICE_UID_STRLEN];
    uint16_t short_addr;
    char msg[128];
    uint8_t payload_flags;
    uint8_t payload_endpoint;
    uint16_t payload_cluster;
    uint16_t payload_attr;
    char payload_cmd[32];
    uint8_t payload_value_type;
    uint8_t payload_value_bool;
    int64_t payload_value_i64;
    double payload_value_f64;
    char payload_value_text[24];
} gw_event_t;

typedef void (*gw_event_bus_listener_t)(const gw_event_t *event, void *user_ctx);

esp_err_t gw_event_bus_init(void);
esp_err_t gw_event_bus_post(gw_event_id_t id, const void *data, size_t data_size, TickType_t ticks_to_wait);

// Lightweight, in-memory event log for UI/debugging.
uint32_t gw_event_bus_last_id(void);
void gw_event_bus_publish(const char *type, const char *source, const char *device_uid, uint16_t short_addr, const char *msg);
// Publish a CBOR payload alongside a human-readable msg.
void gw_event_bus_publish_ex(const char *type,
                            const char *source,
                            const char *device_uid,
                            uint16_t short_addr,
                            const char *msg,
                            const uint8_t *payload_cbor,
                            size_t payload_len);
// Helper: publish a structured CBOR payload without forcing callers to build a human msg.
void gw_event_bus_publish_cbor(const char *type, const char *source, const char *device_uid, uint16_t short_addr, const uint8_t *payload_cbor, size_t payload_len);
// Publish Zigbee events with parsed payload fields (for fast rules evaluation).
void gw_event_bus_publish_zb(const char *type,
                             const char *source,
                             const char *device_uid,
                             uint16_t short_addr,
                             const char *msg,
                             uint8_t endpoint,
                             const char *cmd,
                             uint16_t cluster_id,
                             uint16_t attr_id,
                             gw_event_value_type_t value_type,
                             bool value_bool,
                             int64_t value_i64,
                             double value_f64,
                             const char *value_text,
                             const uint8_t *payload_cbor,
                             size_t payload_len);
size_t gw_event_bus_list_since(uint32_t since_id, gw_event_t *out, size_t max_out, uint32_t *out_last_id);

// Optional listeners called for each gw_event_bus_publish(). Keep callbacks fast and non-blocking.
esp_err_t gw_event_bus_add_listener(gw_event_bus_listener_t cb, void *user_ctx);
esp_err_t gw_event_bus_remove_listener(gw_event_bus_listener_t cb, void *user_ctx);

// Optional async sink for logging + ring updates (owned by another module).
void gw_event_bus_set_out_queue(QueueHandle_t q);
void gw_event_bus_record_event(const gw_event_t *e);

#ifdef __cplusplus
}
#endif

