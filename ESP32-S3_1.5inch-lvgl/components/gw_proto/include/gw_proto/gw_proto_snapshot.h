#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "gw_proto/gw_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generic snapshot streaming helper for gw_proto transports.
 *
 * The transport provides an emit callback.
 * The domain provides a source/iterator over already-materialized proto payloads.
 *
 * This keeps snapshot flow canonical:
 *   SYNC_BEGIN -> zero or more typed records -> SYNC_END
 *
 * Source contract:
 * - rewind() is optional; if present it is called before iteration
 * - next() returns:
 *     ESP_OK          when an item is available
 *     ESP_ERR_NOT_FOUND when iteration is complete
 *     any other error to abort the snapshot
 * - payload pointer must stay valid until emit() returns
 */

typedef esp_err_t (*gw_proto_snapshot_emit_fn)(void *emit_ctx,
                                               uint8_t msg_type,
                                               uint16_t seq,
                                               const void *payload,
                                               uint16_t payload_len);

typedef esp_err_t (*gw_proto_snapshot_rewind_fn)(void *source_ctx);

typedef esp_err_t (*gw_proto_snapshot_next_fn)(void *source_ctx,
                                               uint8_t *out_msg_type,
                                               const void **out_payload,
                                               uint16_t *out_payload_len);

typedef struct {
    void *source_ctx;
    uint32_t total_records;
    gw_proto_snapshot_rewind_fn rewind;
    gw_proto_snapshot_next_fn next;
} gw_proto_snapshot_source_t;

esp_err_t gw_proto_send_snapshot(gw_proto_snapshot_emit_fn emit,
                                 void *emit_ctx,
                                 uint8_t scope,
                                 uint16_t seq,
                                 const gw_proto_snapshot_source_t *source);

#ifdef __cplusplus
}
#endif
