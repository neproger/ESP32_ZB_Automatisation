#pragma once

#include "esp_err.h"
#include "gw_proto/gw_proto.h"
#include "gw_proto/gw_proto_types.h"
#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Canonical C6 Zigbee policy/router entrypoint for typed runtime events.
esp_err_t gw_zigbee_handle_event(const gw_proto_event_v1_t *evt);

// Discovery adapters forward normalized lifecycle/topology milestones here.
esp_err_t gw_zigbee_handle_device_announced(const uint8_t ieee_addr[8], uint16_t short_addr, uint8_t capability);
esp_err_t gw_zigbee_handle_ieee_resolved(const uint8_t ieee_addr[8], uint16_t short_addr);
bool gw_zigbee_handle_quarantine_hit(const gw_device_uid_t *uid, uint16_t short_addr, const char *reason);
void gw_zigbee_handle_discovery_failed(uint16_t short_addr, const char *stage);
bool gw_zigbee_handle_active_ep_discovered(const uint8_t ieee_addr[8],
                                           uint16_t short_addr,
                                           const uint8_t *ep_ids,
                                           uint8_t ep_count);
esp_err_t gw_zigbee_handle_simple_desc_discovered(const uint8_t ieee_addr[8],
                                                  uint16_t short_addr,
                                                  const gw_zb_endpoint_t *ep);
void gw_zigbee_handle_simple_desc_failed(const uint8_t ieee_addr[8],
                                         uint16_t short_addr,
                                         uint8_t endpoint,
                                         uint8_t zdo_status);
void gw_zigbee_handle_simple_desc_bindings(const uint8_t ieee_addr[8],
                                           uint16_t short_addr,
                                           const gw_zb_endpoint_t *ep);
void gw_zigbee_handle_simple_desc_reporting(uint16_t short_addr, const gw_zb_endpoint_t *ep);

#ifdef __cplusplus
}
#endif
