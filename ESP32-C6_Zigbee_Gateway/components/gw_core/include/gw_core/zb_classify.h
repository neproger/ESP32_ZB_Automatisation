#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gw_core/zb_model.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *gw_zb_endpoint_kind(const gw_zb_endpoint_t *ep);
bool gw_zb_endpoint_is_button_like(const gw_zb_endpoint_t *ep);

size_t gw_zb_endpoint_accepts(const gw_zb_endpoint_t *ep, const char **out, size_t max_out);
size_t gw_zb_endpoint_emits(const gw_zb_endpoint_t *ep, const char **out, size_t max_out);
size_t gw_zb_endpoint_reports(const gw_zb_endpoint_t *ep, const char **out, size_t max_out);

#ifdef __cplusplus
}
#endif
