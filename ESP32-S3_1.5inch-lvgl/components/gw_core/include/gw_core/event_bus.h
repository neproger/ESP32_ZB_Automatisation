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
    char payload_value_text[64];
} gw_event_t;

typedef void (*gw_event_bus_listener_t)(const gw_event_t *event, void *user_ctx);

esp_err_t gw_event_bus_init(void);

// Lightweight, in-memory event log for UI/debugging.
uint32_t gw_event_bus_last_id(void);
// Event naming contract:
// - raw/runtime ingress and trace: zigbee.* / rules.* / service-specific debug events
// - canonical model updates: device.* / group.* / settings.* / automation.*
// Service status that should survive and sync belongs in canonical state, not in ad-hoc event names.
void gw_event_bus_publish(const char *type, const char *source, const char *device_uid, uint16_t short_addr, const char *msg);
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
                             const uint8_t *payload_bytes,
                             size_t payload_len);

// Optional listeners called for each gw_event_bus_publish(). Keep callbacks fast and non-blocking.
esp_err_t gw_event_bus_add_listener(gw_event_bus_listener_t cb, void *user_ctx);
esp_err_t gw_event_bus_remove_listener(gw_event_bus_listener_t cb, void *user_ctx);

// Optional async sink for logging + ring updates (owned by another module).
void gw_event_bus_record_event(const gw_event_t *e);

#ifdef __cplusplus
}
#endif

