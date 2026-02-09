#pragma once

#include <stddef.h>

#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// A human-friendly classification for a single endpoint, derived from its Simple Descriptor.
//
// Note: "device type" is profile-specific; this is a practical heuristic based on ZCL clusters
// present on the endpoint (server clusters => accepts commands/reports; client clusters => emits commands).
const char *gw_zb_endpoint_kind(const gw_zb_endpoint_t *ep);

// Returns the list of supported verbs for the endpoint.
// Items are string literals; `out` may be NULL to query required count.
size_t gw_zb_endpoint_accepts(const gw_zb_endpoint_t *ep, const char **out, size_t max_out);
size_t gw_zb_endpoint_emits(const gw_zb_endpoint_t *ep, const char **out, size_t max_out);
size_t gw_zb_endpoint_reports(const gw_zb_endpoint_t *ep, const char **out, size_t max_out);

#ifdef __cplusplus
}
#endif
