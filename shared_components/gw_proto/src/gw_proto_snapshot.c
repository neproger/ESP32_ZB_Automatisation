#include "gw_proto/gw_proto_snapshot.h"

#include <string.h>

esp_err_t gw_proto_send_snapshot(gw_proto_snapshot_emit_fn emit,
                                 void *emit_ctx,
                                 uint8_t scope,
                                 uint16_t seq,
                                 const gw_proto_snapshot_source_t *source)
{
    if (!emit || !source || !source->next || scope == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source->rewind) {
        esp_err_t err = source->rewind(source->source_ctx);
        if (err != ESP_OK) {
            return err;
        }
    }

    gw_proto_sync_begin_v1_t begin = {
        .scope = scope,
        .reserved0 = 0,
        .reserved1 = 0,
        .total_records = source->total_records,
    };
    esp_err_t err = emit(emit_ctx, GW_PROTO_MSG_SYNC_BEGIN, seq, &begin, (uint16_t)sizeof(begin));
    if (err != ESP_OK) {
        return err;
    }

    uint32_t emitted = 0;
    while (true) {
        uint8_t msg_type = GW_PROTO_MSG_NONE;
        const void *payload = NULL;
        uint16_t payload_len = 0;

        err = source->next(source->source_ctx, &msg_type, &payload, &payload_len);
        if (err == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (err != ESP_OK) {
            return err;
        }
        if (msg_type == GW_PROTO_MSG_NONE || (!payload && payload_len != 0)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (emitted >= source->total_records) {
            return ESP_ERR_INVALID_SIZE;
        }

        err = emit(emit_ctx, msg_type, seq, payload, payload_len);
        if (err != ESP_OK) {
            return err;
        }
        emitted++;
    }

    if (emitted != source->total_records) {
        return ESP_ERR_INVALID_SIZE;
    }

    gw_proto_sync_end_v1_t end = {
        .scope = scope,
        .status = 0,
        .reserved0 = 0,
        .total_records = emitted,
    };
    err = emit(emit_ctx, GW_PROTO_MSG_SYNC_END, seq, &end, (uint16_t)sizeof(end));
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}
