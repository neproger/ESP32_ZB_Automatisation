#include "ui_control_ack.hpp"

#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

namespace
{
typedef struct
{
    bool used;
    char device_uid[GW_DEVICE_UID_STRLEN];
    uint8_t endpoint;
    char key[24];
    bool pending;
    bool error;
    uint64_t pending_since_ms;

    bool has_bool;
    bool bool_value;
    bool has_u32;
    uint32_t u32_value;
    bool has_f32;
    float f32_value;
} ack_entry_t;

static constexpr size_t kMaxAckEntries = 96;
static ack_entry_t *s_entries = nullptr;

bool ensure_entries()
{
    if (s_entries) {
        return true;
    }
    s_entries = (ack_entry_t *)heap_caps_calloc(kMaxAckEntries, sizeof(ack_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_entries) {
        s_entries = (ack_entry_t *)calloc(kMaxAckEntries, sizeof(ack_entry_t));
    }
    return s_entries != nullptr;
}

ack_entry_t *find_entry(const char *device_uid, uint8_t endpoint, const char *key)
{
    if (!device_uid || !key || !key[0]) {
        return nullptr;
    }
    if (!ensure_entries()) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxAckEntries; ++i) {
        ack_entry_t *e = &s_entries[i];
        if (!e->used) {
            continue;
        }
        if (e->endpoint != endpoint) {
            continue;
        }
        if (strncmp(e->device_uid, device_uid, sizeof(e->device_uid)) != 0) {
            continue;
        }
        if (strncmp(e->key, key, sizeof(e->key)) != 0) {
            continue;
        }
        return e;
    }
    return nullptr;
}

ack_entry_t *find_or_create_entry(const char *device_uid, uint8_t endpoint, const char *key)
{
    ack_entry_t *e = find_entry(device_uid, endpoint, key);
    if (e) {
        return e;
    }
    if (!ensure_entries()) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxAckEntries; ++i) {
        ack_entry_t *slot = &s_entries[i];
        if (slot->used) {
            continue;
        }
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->endpoint = endpoint;
        strlcpy(slot->device_uid, device_uid, sizeof(slot->device_uid));
        strlcpy(slot->key, key, sizeof(slot->key));
        return slot;
    }
    return nullptr;
}
} // namespace

void ui_control_ack_reset(void)
{
    if (s_entries) {
        memset(s_entries, 0, kMaxAckEntries * sizeof(ack_entry_t));
    }
}

bool ui_control_ack_begin(const char *device_uid, uint8_t endpoint, const char *key)
{
    ack_entry_t *e = find_or_create_entry(device_uid, endpoint, key);
    if (!e) {
        return false;
    }
    if (e->pending) {
        return false;
    }
    e->pending = true;
    e->error = false;
    e->pending_since_ms = 0;
    return true;
}

void ui_control_ack_fail(const char *device_uid, uint8_t endpoint, const char *key)
{
    ack_entry_t *e = find_entry(device_uid, endpoint, key);
    if (!e) {
        return;
    }
    e->pending = false;
    e->error = true;
}

bool ui_control_ack_is_pending(const char *device_uid, uint8_t endpoint, const char *key)
{
    ack_entry_t *e = find_entry(device_uid, endpoint, key);
    if (!e) {
        return false;
    }
    return e->pending;
}

ui_control_ack_status_t ui_control_ack_get_status(const char *device_uid, uint8_t endpoint, const char *key)
{
    ack_entry_t *e = find_entry(device_uid, endpoint, key);
    if (!e) {
        return UI_CONTROL_ACK_IDLE;
    }
    if (e->pending) {
        return UI_CONTROL_ACK_PENDING;
    }
    if (e->error) {
        return UI_CONTROL_ACK_ERROR;
    }
    return UI_CONTROL_ACK_IDLE;
}

void ui_control_ack_poll_timeouts(uint64_t now_ms, uint32_t timeout_ms)
{
    if (!ensure_entries() || timeout_ms == 0) {
        return;
    }
    for (size_t i = 0; i < kMaxAckEntries; ++i) {
        ack_entry_t *e = &s_entries[i];
        if (!e->used || !e->pending) {
            continue;
        }
        if (e->pending_since_ms == 0) {
            e->pending_since_ms = now_ms;
            continue;
        }
        if (now_ms > e->pending_since_ms && (now_ms - e->pending_since_ms) >= timeout_ms) {
            e->pending = false;
            e->error = true;
        }
    }
}

void ui_control_ack_confirm_bool(const char *device_uid, uint8_t endpoint, const char *key, bool has_value, bool value)
{
    ack_entry_t *e = find_or_create_entry(device_uid, endpoint, key);
    if (!e) {
        return;
    }
    e->pending = false;
    e->error = false;
    e->pending_since_ms = 0;
    e->has_bool = has_value;
    e->bool_value = value;
}

bool ui_control_ack_get_confirmed_bool(const char *device_uid, uint8_t endpoint, const char *key, bool *out_has_value, bool *out_value)
{
    ack_entry_t *e = find_entry(device_uid, endpoint, key);
    if (!e) {
        return false;
    }
    if (out_has_value) {
        *out_has_value = e->has_bool;
    }
    if (out_value) {
        *out_value = e->bool_value;
    }
    return true;
}

void ui_control_ack_confirm_u32(const char *device_uid, uint8_t endpoint, const char *key, bool has_value, uint32_t value)
{
    ack_entry_t *e = find_or_create_entry(device_uid, endpoint, key);
    if (!e) {
        return;
    }
    e->pending = false;
    e->error = false;
    e->pending_since_ms = 0;
    e->has_u32 = has_value;
    e->u32_value = value;
}

bool ui_control_ack_get_confirmed_u32(const char *device_uid, uint8_t endpoint, const char *key, bool *out_has_value, uint32_t *out_value)
{
    ack_entry_t *e = find_entry(device_uid, endpoint, key);
    if (!e) {
        return false;
    }
    if (out_has_value) {
        *out_has_value = e->has_u32;
    }
    if (out_value) {
        *out_value = e->u32_value;
    }
    return true;
}

void ui_control_ack_confirm_f32(const char *device_uid, uint8_t endpoint, const char *key, bool has_value, float value)
{
    ack_entry_t *e = find_or_create_entry(device_uid, endpoint, key);
    if (!e) {
        return;
    }
    e->pending = false;
    e->error = false;
    e->pending_since_ms = 0;
    e->has_f32 = has_value;
    e->f32_value = value;
}

bool ui_control_ack_get_confirmed_f32(const char *device_uid, uint8_t endpoint, const char *key, bool *out_has_value, float *out_value)
{
    ack_entry_t *e = find_entry(device_uid, endpoint, key);
    if (!e) {
        return false;
    }
    if (out_has_value) {
        *out_has_value = e->has_f32;
    }
    if (out_value) {
        *out_value = e->f32_value;
    }
    return true;
}
