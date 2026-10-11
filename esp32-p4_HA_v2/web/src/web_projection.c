#include "web/web_projection.h"

#include <string.h>

#include "ha_model/ha_automation.h"
#include "ha_model/ha_capabilities.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_entity.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_logical_device.h"
#include "ha_model/ha_properties.h"
#include "ha_model/ha_rule.h"
#include "ha_model/ha_settings.h"
#include "ha_model/ha_state.h"
#include "ha_model/ha_weather.h"
#include "ha_model/ha_wifi.h"
#include "sys/sys_error.h"
#include "web/web_proto.h"
#include "web/web_sessions.h"

/*
 * Типы, отдаваемые браузеру. Список явный: key/record — canonical struct sizes.
 * `model` = статическая часть (редко меняется → файл/GET); иначе — volatile state.
 */
typedef struct {
    ha_entity_t type;
    uint16_t key_size;
    uint16_t rec_size;
    bool model;
} web_schema_t;

static const web_schema_t SCHEMA[] = {
    {HA_ENTITY_LOGICAL_DEVICE, sizeof(ha_logical_device_key_t),
     sizeof(ha_logical_device_record_t), true},
    {HA_ENTITY_ENTITY, sizeof(ha_entity_key_t), sizeof(ha_entity_record_t), true},
    {HA_ENTITY_PROPERTY_STATE, sizeof(ha_state_key_t), sizeof(ha_state_record_t), false},
    {HA_ENTITY_CAPABILITIES, sizeof(ha_entity_key_t), sizeof(ha_entity_capabilities_record_t),
     true},
    {HA_ENTITY_AUTOMATION, sizeof(ha_automation_key_t), sizeof(ha_rule_t), true},
    {HA_ENTITY_GROUP_ITEM, sizeof(ha_group_item_key_t), sizeof(ha_group_item_record_t), true},
    {HA_ENTITY_GROUP, sizeof(ha_group_key_t), sizeof(ha_group_record_t), true},
    {HA_ENTITY_LOCATION, sizeof(ha_device_id_t), sizeof(ha_location_record_t), true},
    {HA_ENTITY_WEATHER, sizeof(ha_device_id_t), sizeof(ha_weather_record_t), false},
    {HA_ENTITY_WIFI_STATUS, sizeof(ha_device_id_t), sizeof(ha_wifi_status_record_t), false},
    {HA_ENTITY_SETTINGS, sizeof(ha_settings_key_t), sizeof(ha_settings_record_t), true},
};
#define SCHEMA_COUNT (sizeof(SCHEMA) / sizeof(SCHEMA[0]))

static domain_t *s_domain;

static const web_schema_t *schema_for(uint32_t type)
{
    for (size_t i = 0; i < SCHEMA_COUNT; i++) {
        if ((uint32_t)SCHEMA[i].type == type) {
            return &SCHEMA[i];
        }
    }
    return NULL;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

/* --- snapshot ----------------------------------------------------------- */

static bool scan_count(const void *key, const void *record, void *ctx)
{
    (void)key;
    (void)record;
    (*(uint32_t *)ctx)++;
    return true;
}

typedef struct {
    int fd;
    uint8_t type;
    uint16_t key_size;
    uint16_t rec_size;
    uint32_t sent;
} snapshot_ctx_t;

static bool scan_emit(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    uint8_t out[WEB_PROTO_MAX_PAYLOAD];
    memcpy(out, key, s->key_size);
    memcpy(out + s->key_size, record, s->rec_size);
    web_sessions_send(s->fd, s->type, out, (uint16_t)(s->key_size + s->rec_size));
    s->sent++;
    return true;
}

void web_projection_snapshot(int fd)
{
    uint32_t total = 0;
    for (size_t i = 0; i < SCHEMA_COUNT; i++) {
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, scan_count, &total);
    }

    uint8_t word[4];
    put_u32(word, total);
    web_sessions_send(fd, WEB_MSG_SYNC_BEGIN, word, sizeof(word));

    uint32_t sent = 0;
    for (size_t i = 0; i < SCHEMA_COUNT; i++) {
        snapshot_ctx_t ctx = {.fd = fd,
                              .type = (uint8_t)SCHEMA[i].type,
                              .key_size = SCHEMA[i].key_size,
                              .rec_size = SCHEMA[i].rec_size,
                              .sent = 0};
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, scan_emit, &ctx);
        sent += ctx.sent;
    }

    put_u32(word, sent);
    web_sessions_send(fd, WEB_MSG_SYNC_END, word, sizeof(word));
}

/* --- классификация типов + MODEL-блоб + STATE-snapshot ------------------- */

bool web_projection_is_model(domain_entity_t type)
{
    for (size_t i = 0; i < SCHEMA_COUNT; i++) {
        if ((uint32_t)SCHEMA[i].type == (uint32_t)type) {
            return SCHEMA[i].model;
        }
    }
    return false;
}

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t off;
    uint8_t type;
    uint16_t key_size;
    uint16_t rec_size;
    uint32_t count;
    bool overflow;
} blob_ctx_t;

static bool blob_emit(const void *key, const void *record, void *ctx)
{
    blob_ctx_t *w = (blob_ctx_t *)ctx;
    const size_t need = (size_t)1 + w->key_size + w->rec_size;
    if (w->off + need > w->cap) {
        w->overflow = true;
        return false;
    }
    w->buf[w->off++] = w->type;
    memcpy(w->buf + w->off, key, w->key_size);
    w->off += w->key_size;
    memcpy(w->buf + w->off, record, w->rec_size);
    w->off += w->rec_size;
    w->count++;
    return true;
}

size_t web_projection_build_model(uint8_t *buf, size_t cap, uint32_t *out_count)
{
    blob_ctx_t w = {.buf = buf, .cap = cap, .off = 0, .count = 0, .overflow = false};
    for (size_t i = 0; i < SCHEMA_COUNT; i++) {
        if (!SCHEMA[i].model) {
            continue;
        }
        w.type = (uint8_t)SCHEMA[i].type;
        w.key_size = SCHEMA[i].key_size;
        w.rec_size = SCHEMA[i].rec_size;
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, blob_emit, &w);
        if (w.overflow) {
            break;
        }
    }
    if (out_count != NULL) {
        *out_count = w.count;
    }
    return w.overflow ? 0 : w.off;
}

static bool state_emit(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    uint8_t out[WEB_PROTO_MAX_PAYLOAD];
    memcpy(out, key, s->key_size);
    memcpy(out + s->key_size, record, s->rec_size);
    web_sessions_send(s->fd, s->type, out, (uint16_t)(s->key_size + s->rec_size));
    s->sent++;
    return true;
}

void web_projection_send_state(int fd)
{
    uint8_t word[4];
    put_u32(word, 0);
    web_sessions_send(fd, WEB_MSG_SYNC_BEGIN, word, sizeof(word));
    for (size_t i = 0; i < SCHEMA_COUNT; i++) {
        if (SCHEMA[i].model) {
            continue;
        }
        snapshot_ctx_t ctx = {.fd = fd,
                              .type = (uint8_t)SCHEMA[i].type,
                              .key_size = SCHEMA[i].key_size,
                              .rec_size = SCHEMA[i].rec_size,
                              .sent = 0};
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, state_emit, &ctx);
    }
    put_u32(word, 0);
    web_sessions_send(fd, WEB_MSG_SYNC_END, word, sizeof(word));
}

/* --- delta -------------------------------------------------------------- */

/* EVENT (канонический факт): payload = ha_event_t verbatim. */
static void send_event(const domain_event_t *event)
{
    if (event->entity != (uint32_t)HA_ENTITY_ENTITY || event->payload_ref == 0 ||
        event->payload_size != sizeof(ha_event_t)) {
        return;
    }
    ha_event_t ev = {0};
    if (sys_failed(domain_payload_get(s_domain, event->payload_ref, &ev, sizeof(ev)))) {
        return; /* payload вытеснен — событие не несём (редкое, best-effort) */
    }
    web_sessions_broadcast(WEB_MSG_EVENT, &ev, sizeof(ev));
}

/* UPSERT/REMOVE по canonical типу: [type][key][record] / [type][key]. */
void web_projection_on_event(const domain_event_t *event)
{
    if (event->kind == (uint8_t)DOMAIN_FACT_EVENT) {
        send_event(event);
        return;
    }

    const web_schema_t *s = schema_for(event->entity);
    if (s == NULL || event->key_size != s->key_size) {
        return;
    }

    uint8_t out[WEB_PROTO_MAX_PAYLOAD];
    if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
        if (sys_failed(domain_entity_get(s_domain, event->entity, event->key,
                                         out + s->key_size))) {
            return;
        }
        memcpy(out, event->key, s->key_size);
        web_sessions_broadcast((uint8_t)s->type, out, (uint16_t)(s->key_size + s->rec_size));
    } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
        web_sessions_broadcast((uint8_t)s->type, event->key, s->key_size);
    }
}

void web_projection_init(domain_t *domain)
{
    s_domain = domain;
}
