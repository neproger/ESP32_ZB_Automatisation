#pragma once

#include "esp_err.h"

#include "gw_core/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_PROTO_BUS_CHANNEL_INGRESS = 1 << 0,
    GW_PROTO_BUS_CHANNEL_MODEL   = 1 << 1,
    GW_PROTO_BUS_CHANNEL_TRACE   = 1 << 2,
    GW_PROTO_BUS_CHANNEL_ANY     = 0xFF,
} gw_proto_bus_channel_t;

typedef void (*gw_proto_bus_listener_t)(gw_proto_bus_channel_t channel,
                                        const gw_proto_hdr_t *hdr,
                                        const void *payload,
                                        void *user_ctx);

esp_err_t gw_proto_bus_init(void);
esp_err_t gw_proto_bus_add_listener(gw_proto_bus_listener_t cb, uint8_t channel_mask, void *user_ctx);
esp_err_t gw_proto_bus_remove_listener(gw_proto_bus_listener_t cb, void *user_ctx);
esp_err_t gw_proto_bus_publish(gw_proto_bus_channel_t channel, const gw_proto_hdr_t *hdr, const void *payload);

#ifdef __cplusplus
}
#endif
