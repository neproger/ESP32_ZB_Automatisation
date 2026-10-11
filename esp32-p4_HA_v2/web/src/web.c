#include "web/web.h"

#include <math.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ha_model/ha_automation.h"
#include "ha_model/ha_capabilities.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "device/device_service.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_logical_device.h"
#include "ha_model/ha_rule.h"
#include "ha_model/ha_settings.h"
#include "ha_model/ha_state.h"
#include "ha_model/ha_weather.h"
#include "ha_model/ha_wifi.h"
#include "semantics/semantics.h"
#include "web/web_proto.h"

/*
 * Web service (docs/services/WEB.md, docs/services/WEB_PROTOCOL.md).
 *
 * Wi-Fi не его: радио на внешнем C3 держит сервис `wifi` (docs/services/WIFI.md);
 * web — только HTTP+WS. На проводе — сырые записи Domain как есть (вариант A):
 * никаких DTO/строк.
 *
 * Задача web:
 *   - поднимает HTTP+WS (сервер стартует и без IP, доступен после подключения);
 *   - на подключение клиента шлёт snapshot (SYNC_BEGIN → ENTITY… → SYNC_END);
 *   - подписана на Domain и рассылает дельты (ENTITY / ENTITY_REMOVE) всем клиентам;
 *   - выполняет команды из WS (Zigbee-команда, CRUD автоматизаций, переименование).
 */

static const char *TAG = "web";

#define WEB_TASK_STACK 6144
#define WEB_TASK_PRIORITY 4
#define WEB_MAX_CLIENTS 8
#define WEB_INBOX_LENGTH 32
/* Максимальный входящий кадр целиком (заголовок + payload), не только команда. */
#define WEB_CMD_BUF WEB_PROTO_MAX_FRAME

_Static_assert(WEB_ENTITY_AUTOMATION == (uint32_t)HA_ENTITY_AUTOMATION, "entity id: automation");
_Static_assert(WEB_ENTITY_LOCATION == (uint32_t)HA_ENTITY_LOCATION, "entity id: location");
_Static_assert(WEB_ENTITY_GROUP == (uint32_t)HA_ENTITY_GROUP, "entity id: group");
_Static_assert(WEB_ENTITY_GROUP_ITEM == (uint32_t)HA_ENTITY_GROUP_ITEM, "entity id: group-item");
_Static_assert(WEB_ENTITY_WEATHER == (uint32_t)HA_ENTITY_WEATHER, "entity id: weather");
_Static_assert(WEB_ENTITY_WIFI_STATUS == (uint32_t)HA_ENTITY_WIFI_STATUS, "entity id: wifi-status");
_Static_assert(WEB_ENTITY_SETTINGS == (uint32_t)HA_ENTITY_SETTINGS, "entity id: settings");

/* Схема записи: браузер знает те же размеры (web-ui/src/schema.js). */
typedef struct {
    uint8_t type;
    uint16_t key_size;
    uint16_t rec_size;
} web_schema_t;

/*
 * Список типов на проводе задан один раз (X-macro): из него генерируются и строки
 * таблицы, и компайл-тайм проверка, что запись (1 байт типа + key + record) влезает в
 * один кадр. Без неё рост записи молча переполнил бы буфер snapshot/дельты (так было,
 * когда automation вырос до 144 при буфере под endpoint 72).
 */
#define WEB_SCHEMA_LIST(X)                                                                  \
    X(WEB_ENTITY_LOCATION, sizeof(ha_device_id_t), sizeof(ha_location_record_t))           \
    X(WEB_ENTITY_GROUP, sizeof(ha_group_key_t), sizeof(ha_group_record_t))                  \
    X(WEB_ENTITY_WEATHER, sizeof(ha_device_id_t), sizeof(ha_weather_record_t))             \
    X(WEB_ENTITY_WIFI_STATUS, sizeof(ha_device_id_t), sizeof(ha_wifi_status_record_t))     \
    X(WEB_ENTITY_SETTINGS, sizeof(ha_settings_key_t), sizeof(ha_settings_record_t))

#define WEB_SCHEMA_FITS(type, key_size, rec_size)                                  \
    _Static_assert((1u + (key_size) + (rec_size)) <= WEB_PROTO_MAX_PAYLOAD,        \
                   "web schema record exceeds WEB_PROTO_MAX_PAYLOAD: " #type);
WEB_SCHEMA_LIST(WEB_SCHEMA_FITS)

#define WEB_SCHEMA_ROW(type, key_size, rec_size) \
    { (type), (uint16_t)(key_size), (uint16_t)(rec_size) },
static const web_schema_t SCHEMA[] = {WEB_SCHEMA_LIST(WEB_SCHEMA_ROW)};
#undef WEB_SCHEMA_ROW

#define WEB_SCHEMA_COUNT (sizeof(SCHEMA) / sizeof(SCHEMA[0]))

/* Элемент inbox задачи: факт от Domain либо запрос snapshot конкретному fd. */
typedef struct {
    uint8_t kind; /* 0 — факт, 1 — snapshot */
    int fd;
    domain_event_t event;
} web_inbox_item_t;

static domain_t *s_domain;
static httpd_handle_t s_server;
static QueueHandle_t s_inbox;

static const web_schema_t *schema_for(uint8_t type)
{
    for (size_t i = 0; i < WEB_SCHEMA_COUNT; i++) {
        if (SCHEMA[i].type == type) {
            return &SCHEMA[i];
        }
    }
    return NULL;
}

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static void put_u64(uint8_t *out, uint64_t value)
{
    put_u32(out, (uint32_t)value);
    put_u32(out + 4, (uint32_t)(value >> 32));
}

/* Стабильный wire-кодек семантического скаляра: u8 kind + u32 bits (общий для
 * command ingress и property state). bits трактуется по kind. */
static bool bits_to_value(ha_value_kind_t kind, uint32_t bits, ha_value_t *out)
{
    out->kind = kind;
    switch (kind) {
    case HA_VALUE_BOOL:
        out->value.b = bits != 0;
        return true;
    case HA_VALUE_I32:
        out->value.i32 = (int32_t)bits;
        return true;
    case HA_VALUE_U32:
        out->value.u32 = bits;
        return true;
    case HA_VALUE_ENUM:
        out->value.enum_value = bits;
        return true;
    case HA_VALUE_FLOAT: {
        float f = 0.0f;
        memcpy(&f, &bits, sizeof(f));
        if (!isfinite(f)) { /* граница доверия: NaN/Inf не проходит */
            return false;
        }
        out->value.f32 = f;
        return true;
    }
    default:
        return false;
    }
}

static uint32_t value_to_bits(const ha_value_t *value)
{
    switch (value->kind) {
    case HA_VALUE_BOOL:
        return value->value.b ? 1u : 0u;
    case HA_VALUE_I32:
        return (uint32_t)value->value.i32;
    case HA_VALUE_U32:
        return value->value.u32;
    case HA_VALUE_ENUM:
        return value->value.enum_value;
    case HA_VALUE_FLOAT: {
        uint32_t bits = 0;
        memcpy(&bits, &value->value.f32, sizeof(bits));
        return bits;
    }
    default:
        return 0;
    }
}

/* Общий wire-кодек entity-правила (определены ниже). */
static uint16_t web_rule_encode(const ha_automation_key_t *key, const ha_rule_t *rule, uint8_t *out,
                                uint16_t *out_len);
static uint16_t web_rule_decode(const uint8_t *a, size_t len, ha_automation_key_t *key,
                                ha_rule_t *out, size_t *used);

static void web_send_frame(int fd, uint8_t type, uint16_t seq, const void *payload, uint16_t len)
{
    uint8_t frame[WEB_PROTO_MAX_FRAME];
    const size_t n = web_frame_encode(frame, sizeof(frame), type, seq, payload, len);
    if (n == 0) {
        return;
    }
    httpd_ws_frame_t ws = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = frame,
        .len = n,
    };
    /* Несмотря на имя, шлёт синхронно: payload нужен только на время вызова. */
    httpd_ws_send_frame_async(s_server, fd, &ws);
}

static void web_broadcast(uint8_t type, uint16_t seq, const void *payload, uint16_t len)
{
    int fds[WEB_MAX_CLIENTS];
    size_t count = WEB_MAX_CLIENTS;
    if (httpd_get_client_list(s_server, &count, fds) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            web_send_frame(fds[i], type, seq, payload, len);
        }
    }
}

/* --- canonical v3 (A6.1): identity = entity_id, без uid/endpoint/cluster/attr ----- */

static bool web3_entity_payload(ha_entity_id_t entity_id, ha_device_id_t device_id,
                                const char *name, uint8_t *out, uint16_t *out_len)
{
    put_u64(out, entity_id);
    put_u64(out + 8, device_id);
    memset(out + 16, 0, HA_ENTITY_NAME_MAX);
    if (name != NULL) {
        const size_t n = strnlen(name, HA_ENTITY_NAME_MAX);
        memcpy(out + 16, name, n);
    }
    *out_len = (uint16_t)WEB_MSG3_ENTITY_SIZE;
    return true;
}

/* Logical Device (B5): device_id + parent_device_id + name (для группировки/дерева). */
static bool web3_device_payload(ha_device_id_t device_id,
                                const ha_logical_device_record_t *record, uint8_t *out,
                                uint16_t *out_len)
{
    put_u64(out, device_id);
    put_u64(out + 8, record->parent_device_id);
    memset(out + 16, 0, HA_LOGICAL_DEVICE_NAME_MAX);
    const size_t n = strnlen(record->name, HA_LOGICAL_DEVICE_NAME_MAX);
    memcpy(out + 16, record->name, n);
    *out_len = (uint16_t)WEB_MSG3_DEVICE_SIZE;
    return true;
}

static bool web3_property_state_payload(ha_entity_id_t entity_id, ha_property_id_t property,
                                        const ha_state_record_t *record, uint8_t *out,
                                        uint16_t *out_len)
{
    put_u64(out, entity_id);
    put_u16(out + 8, (uint16_t)property);
    out[10] = (uint8_t)record->value.kind;
    put_u32(out + 11, value_to_bits(&record->value));
    *out_len = (uint16_t)WEB_MSG3_PROPERTY_STATE_SIZE;
    return true;
}

static bool web3_property_state_remove_payload(ha_entity_id_t entity_id, ha_property_id_t property,
                                               uint8_t *out, uint16_t *out_len)
{
    put_u64(out, entity_id);
    put_u16(out + 8, (uint16_t)property);
    *out_len = (uint16_t)WEB_MSG3_PROPERTY_STATE_REMOVE_SIZE;
    return true;
}

/*
 * CAPABILITIES (перем.): u64 entity_id @0; u8 count @8; props[]{u16 property,u8 n,u8 actions[]};
 * затем u8 event_count + u8 events[] (press-события client-кластеров).
 */
static bool web3_capabilities_payload(ha_entity_id_t entity_id,
                                      const ha_entity_capabilities_record_t *record, uint8_t *out,
                                      uint16_t *out_len)
{
    put_u64(out, entity_id);
    size_t o = 8;
    out[o++] = record->property_count;
    for (uint8_t i = 0; i < record->property_count && i < HA_ENTITY_CAPABILITIES_MAX; i++) {
        const ha_capability_t *cap = &record->properties[i];
        put_u16(out + o, (uint16_t)cap->property);
        o += 2;
        out[o++] = cap->action_count;
        for (uint8_t a = 0; a < cap->action_count && a < HA_CAPABILITY_ACTIONS_MAX; a++) {
            out[o++] = (uint8_t)cap->actions[a];
        }
    }
    out[o++] = record->event_count;
    for (uint8_t e = 0; e < record->event_count && e < HA_ENTITY_EVENTS_MAX; e++) {
        out[o++] = (uint8_t)record->events[e];
    }
    *out_len = (uint16_t)o;
    return true;
}

/*
 * Canonical group_item → wire (A4): identity уже entity/property, мост не нужен.
 * ITEM (52): u64 group_id @0, u64 entity @8, u16 property @16, u16 order @18, title[32] @20.
 */
static bool group_item_payload(const ha_group_item_key_t *key,
                                        const ha_group_item_record_t *record, uint8_t *out,
                                        uint16_t *out_len)
{
    put_u64(out, key->group_id);
    put_u64(out + 8, key->state.entity);
    put_u16(out + 16, (uint16_t)key->state.property);
    put_u16(out + 18, record->order);
    memcpy(out + 20, record->title, HA_GROUP_ITEM_TITLE_MAX);
    *out_len = 20 + HA_GROUP_ITEM_TITLE_MAX;
    return true;
}

/* REMOVE (18): u64 group_id, u64 entity, u16 property. */
static bool group_item_remove_payload(const ha_group_item_key_t *key, uint8_t *out,
                                               uint16_t *out_len)
{
    put_u64(out, key->group_id);
    put_u64(out + 8, key->state.entity);
    put_u16(out + 16, (uint16_t)key->state.property);
    *out_len = 18;
    return true;
}

/* --- snapshot ----------------------------------------------------------- */

static bool snapshot_count(const void *key, const void *record, void *ctx)
{
    (void)key;
    (void)record;
    (*(uint32_t *)ctx)++;
    return true;
}

typedef struct {
    int fd;
    const web_schema_t *schema;
    uint32_t *count;
} snapshot_ctx_t;

static bool snapshot_emit(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    uint8_t payload[WEB_PROTO_MAX_PAYLOAD];
    payload[0] = s->schema->type;
    memcpy(payload + 1, key, s->schema->key_size);
    memcpy(payload + 1 + s->schema->key_size, record, s->schema->rec_size);
    web_send_frame(s->fd, WEB_MSG_ENTITY, 0, payload,
                   (uint16_t)(1 + s->schema->key_size + s->schema->rec_size));
    (*s->count)++;
    return true;
}

/* canonical group_item (A4): identity = (group, entity, property). */
static bool snapshot_emit_group_item(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    uint8_t out[WEB_PROTO_MAX_PAYLOAD];
    uint16_t len = 0;
    if (group_item_payload((const ha_group_item_key_t *)key,
                                    (const ha_group_item_record_t *)record, out, &len)) {
        web_send_frame(s->fd, WEB_MSG_GROUP_ITEM, 0, out, len);
    }
    (*s->count)++;
    return true;
}

/* canonical v3: ENTITY и PROPERTY_STATE не входят в v2 SCHEMA — отдельные эмиттеры. */
static bool snapshot_emit_entity3(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    const ha_entity_record_t *rec = (const ha_entity_record_t *)record;
    uint8_t out[WEB_MSG3_ENTITY_SIZE];
    uint16_t len = 0;
    if (web3_entity_payload(*(const ha_entity_id_t *)key, rec->device_id, rec->name, out, &len)) {
        web_send_frame(s->fd, WEB_MSG3_ENTITY, 0, out, len);
    }
    (*s->count)++;
    return true;
}

/* Logical Device (B5): группировка Device → Entity. */
static bool snapshot_emit_device3(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    uint8_t out[WEB_MSG3_DEVICE_SIZE];
    uint16_t len = 0;
    if (web3_device_payload(*(const ha_device_id_t *)key,
                            (const ha_logical_device_record_t *)record, out, &len)) {
        web_send_frame(s->fd, WEB_MSG3_DEVICE, 0, out, len);
    }
    (*s->count)++;
    return true;
}

static bool snapshot_emit_property3(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    const ha_state_key_t *k = (const ha_state_key_t *)key;
    uint8_t out[WEB_MSG3_PROPERTY_STATE_SIZE];
    uint16_t len = 0;
    if (web3_property_state_payload(k->entity, k->property, (const ha_state_record_t *)record, out,
                                    &len)) {
        web_send_frame(s->fd, WEB_MSG3_PROPERTY_STATE, 0, out, len);
    }
    (*s->count)++;
    return true;
}

static bool snapshot_emit_capabilities3(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    uint8_t out[WEB_PROTO_MAX_PAYLOAD];
    uint16_t len = 0;
    if (web3_capabilities_payload(*(const ha_entity_id_t *)key,
                                  (const ha_entity_capabilities_record_t *)record, out, &len)) {
        web_send_frame(s->fd, WEB_MSG3_CAPABILITIES, 0, out, len);
    }
    (*s->count)++;
    return true;
}

static bool snapshot_emit_automation3(const void *key, const void *record, void *ctx)
{
    snapshot_ctx_t *s = (snapshot_ctx_t *)ctx;
    uint8_t out[WEB_PROTO_MAX_PAYLOAD];
    uint16_t len = 0;
    if (web_rule_encode((const ha_automation_key_t *)key, (const ha_rule_t *)record, out, &len) ==
        (uint16_t)SYS_CODE_OK) {
        web_send_frame(s->fd, WEB_MSG3_AUTOMATION, 0, out, len);
    }
    (*s->count)++;
    return true;
}

static void web_send_snapshot(int fd)
{
    uint32_t total = 0;
    for (size_t i = 0; i < WEB_SCHEMA_COUNT; i++) {
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, snapshot_count, &total);
    }
    /* canonical v3 */
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_ENTITY, snapshot_count, &total);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE, snapshot_count, &total);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, snapshot_count, &total);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_CAPABILITIES, snapshot_count, &total);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION, snapshot_count, &total);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, snapshot_count, &total);

    uint8_t word[4];
    put_u32(word, total);
    web_send_frame(fd, WEB_MSG_SYNC_BEGIN, 0, word, sizeof(word));

    uint32_t sent = 0;
    for (size_t i = 0; i < WEB_SCHEMA_COUNT; i++) {
        snapshot_ctx_t ctx = {.fd = fd, .schema = &SCHEMA[i], .count = &sent};
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, snapshot_emit, &ctx);
    }

    snapshot_ctx_t v3 = {.fd = fd, .schema = NULL, .count = &sent};
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_ENTITY, snapshot_emit_entity3, &v3);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE, snapshot_emit_device3,
                       &v3);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE,
                       snapshot_emit_property3, &v3);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_CAPABILITIES,
                       snapshot_emit_capabilities3, &v3);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION,
                       snapshot_emit_automation3, &v3);
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, snapshot_emit_group_item,
                       &v3);

    put_u32(word, sent);
    web_send_frame(fd, WEB_MSG_SYNC_END, 0, word, sizeof(word));
}

/* --- delta -------------------------------------------------------------- */

/*
 * Canonical EVENT (A6.2) → WEB_MSG3_EVENT: источник — Entity (key = entity_id), без
 * uid/endpoint/raw payload. DTO (14): u64 source_entity @0, u8 event_id @8,
 * u8 value_kind @9, u32 value_bits @10. Legacy DEVICE-keyed события игнорируем.
 */
static void web_send_event(const domain_event_t *event)
{
    if (event->entity != (uint32_t)HA_ENTITY_ENTITY ||
        event->key_size != sizeof(ha_entity_id_t)) {
        return;
    }
    uint8_t out[14] = {0};
    memcpy(out, event->key, sizeof(ha_entity_id_t));
    out[8] = (event->value.type == (uint8_t)DOMAIN_VALUE_ENUM) ? (uint8_t)event->value.v.u32
                                                               : (uint8_t)HA_EVENT_NONE;
    out[9] = (uint8_t)HA_VALUE_NONE; /* payload события пока не несём */
    web_broadcast(WEB_MSG3_EVENT, 0, out, sizeof(out));
}

static void web_send_fact(const domain_event_t *event)
{
    if (event->kind == (uint8_t)DOMAIN_FACT_EVENT) {
        web_send_event(event);
        return;
    }

    /* canonical v3 (A6.1): identity = entity_id. */
    if (event->entity == (uint32_t)HA_ENTITY_ENTITY && event->key_size == sizeof(ha_entity_id_t)) {
        const ha_entity_id_t entity_id = *(const ha_entity_id_t *)event->key;
        uint8_t out[WEB_MSG3_ENTITY_SIZE];
        uint16_t len = 0;
        if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
            ha_entity_record_t record = {0};
            if (sys_ok(domain_entity_get(s_domain, event->entity, event->key, &record)) &&
                web3_entity_payload(entity_id, record.device_id, record.name, out, &len)) {
                web_broadcast(WEB_MSG3_ENTITY, 0, out, len);
            }
        } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
            (void)web3_entity_payload(entity_id, HA_DEVICE_ID_NONE, NULL, out, &len);
            web_broadcast(WEB_MSG3_ENTITY_REMOVE, 0, out, len);
        }
        return;
    }
    if (event->entity == (uint32_t)HA_ENTITY_LOGICAL_DEVICE &&
        event->key_size == sizeof(ha_logical_device_key_t)) {
        const ha_device_id_t device_id = *(const ha_device_id_t *)event->key;
        uint8_t out[WEB_MSG3_DEVICE_SIZE];
        uint16_t len = 0;
        if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
            ha_logical_device_record_t record = {0};
            if (sys_ok(domain_entity_get(s_domain, event->entity, event->key, &record)) &&
                web3_device_payload(device_id, &record, out, &len)) {
                web_broadcast(WEB_MSG3_DEVICE, 0, out, len);
            }
        } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
            put_u64(out, device_id);
            web_broadcast(WEB_MSG3_DEVICE_REMOVE, 0, out, 8);
        }
        return;
    }
    if (event->entity == (uint32_t)HA_ENTITY_PROPERTY_STATE &&
        event->key_size == sizeof(ha_state_key_t)) {
        const ha_state_key_t *key = (const ha_state_key_t *)event->key;
        uint8_t out[WEB_MSG3_PROPERTY_STATE_SIZE];
        uint16_t len = 0;
        if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
            ha_state_record_t record = {0};
            if (sys_ok(domain_entity_get(s_domain, event->entity, event->key, &record)) &&
                web3_property_state_payload(key->entity, key->property, &record, out, &len)) {
                web_broadcast(WEB_MSG3_PROPERTY_STATE, 0, out, len);
            }
        } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED &&
                   web3_property_state_remove_payload(key->entity, key->property, out, &len)) {
            web_broadcast(WEB_MSG3_PROPERTY_STATE_REMOVE, 0, out, len);
        }
        return;
    }
    if (event->entity == (uint32_t)HA_ENTITY_CAPABILITIES &&
        event->key_size == sizeof(ha_entity_key_t)) {
        const ha_entity_id_t entity_id = ((const ha_entity_key_t *)event->key)->id;
        if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
            ha_entity_capabilities_record_t record = {0};
            if (sys_ok(domain_entity_get(s_domain, event->entity, event->key, &record))) {
                uint8_t out[WEB_PROTO_MAX_PAYLOAD];
                uint16_t len = 0;
                if (web3_capabilities_payload(entity_id, &record, out, &len)) {
                    web_broadcast(WEB_MSG3_CAPABILITIES, 0, out, len);
                }
            }
        } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
            uint8_t out[8] = {0};
            put_u64(out, entity_id);
            web_broadcast(WEB_MSG3_CAPABILITIES_REMOVE, 0, out, sizeof(out));
        }
        return;
    }
    if (event->entity == (uint32_t)HA_ENTITY_AUTOMATION &&
        event->key_size == sizeof(ha_automation_key_t)) {
        if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
            ha_rule_t rule = {0};
            if (sys_ok(domain_entity_get(s_domain, event->entity, event->key, &rule))) {
                uint8_t out[WEB_PROTO_MAX_PAYLOAD];
                uint16_t len = 0;
                if (web_rule_encode((const ha_automation_key_t *)event->key, &rule, out, &len) ==
                    (uint16_t)SYS_CODE_OK) {
                    web_broadcast(WEB_MSG3_AUTOMATION, 0, out, len);
                }
            }
        } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
            web_broadcast(WEB_MSG3_AUTOMATION_REMOVE, 0, event->key,
                          (uint16_t)sizeof(ha_automation_key_t));
        }
        return;
    }
    if (event->entity == (uint32_t)HA_ENTITY_GROUP_ITEM &&
        event->key_size == sizeof(ha_group_item_key_t)) {
        if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
            ha_group_item_record_t record = {0};
            if (sys_ok(domain_entity_get(s_domain, event->entity, event->key, &record))) {
                uint8_t out[WEB_PROTO_MAX_PAYLOAD];
                uint16_t len = 0;
                if (group_item_payload((const ha_group_item_key_t *)event->key, &record,
                                                out, &len)) {
                    web_broadcast(WEB_MSG_GROUP_ITEM, 0, out, len);
                }
            }
        } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
            uint8_t out[WEB_PROTO_MAX_PAYLOAD];
            uint16_t len = 0;
            if (group_item_remove_payload((const ha_group_item_key_t *)event->key, out,
                                                   &len)) {
                web_broadcast(WEB_MSG_GROUP_ITEM_REMOVE, 0, out, len);
            }
        }
        return;
    }

    const web_schema_t *schema = schema_for((uint8_t)event->entity);
    if (schema == NULL || event->key_size != schema->key_size) {
        return;
    }

    uint8_t payload[WEB_PROTO_MAX_PAYLOAD];
    payload[0] = schema->type;
    memcpy(payload + 1, event->key, schema->key_size);

    if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED) {
        uint8_t record[WEB_PROTO_MAX_PAYLOAD];
        if (sys_failed(domain_entity_get(s_domain, event->entity, event->key, record))) {
            return;
        }
        memcpy(payload + 1 + schema->key_size, record, schema->rec_size);
        web_broadcast(WEB_MSG_ENTITY, 0, payload,
                      (uint16_t)(1 + schema->key_size + schema->rec_size));
    } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
        web_broadcast(WEB_MSG_ENTITY_REMOVE, 0, payload, (uint16_t)(1 + schema->key_size));
    }
}

/* --- команды ------------------------------------------------------------ */

/* Canonical команда по entity (A6.1c): строим ha_property_command_t и постим HA_CMD_PROPERTY. */
static uint16_t web_do_property_command(const uint8_t *args, size_t len)
{
    if (len < 12) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_property_command_t request = {0};
    request.entity = get_u64(args + 0);
    request.property = (ha_property_id_t)get_u16(args + 8);
    request.action = args[10];
    const uint8_t value_kind = args[11];

    if (value_kind == (uint8_t)HA_COMMAND_VALUE_NONE) {
        if (len != 12) {
            return SYS_CODE_INVALID_SIZE;
        }
    } else if (value_kind == (uint8_t)HA_COMMAND_VALUE_SCALAR) {
        if (len != 17) {
            return SYS_CODE_INVALID_SIZE;
        }
        request.value.kind = HA_COMMAND_VALUE_SCALAR;
        if (!bits_to_value((ha_value_kind_t)args[12], get_u32(args + 13), &request.value.value.scalar)) {
            return SYS_CODE_INVALID_ARG;
        }
    } else if (value_kind == (uint8_t)HA_COMMAND_VALUE_XY) {
        if (len != 20) {
            return SYS_CODE_INVALID_SIZE;
        }
        uint32_t xb = get_u32(args + 12);
        uint32_t yb = get_u32(args + 16);
        float x = 0.0f;
        float y = 0.0f;
        memcpy(&x, &xb, sizeof(x));
        memcpy(&y, &yb, sizeof(y));
        if (!isfinite(x) || !isfinite(y)) {
            return SYS_CODE_INVALID_ARG;
        }
        request.value.kind = HA_COMMAND_VALUE_XY;
        request.value.value.xy.x = x;
        request.value.value.xy.y = y;
    } else {
        return SYS_CODE_INVALID_ARG;
    }

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err =
        domain_post(s_domain, HA_CMD_PROPERTY, &request, sizeof(request), NULL, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Rename Logical Device (B6): u64 device_id + char name[32]; через Device service. */
static uint16_t web_do_device_rename(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_id_t) + HA_LOGICAL_DEVICE_NAME_MAX) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_id_t device_id = 0;
    memcpy(&device_id, args, sizeof(device_id));
    char name[HA_LOGICAL_DEVICE_NAME_MAX + 1] = {0};
    memcpy(name, args + sizeof(device_id), HA_LOGICAL_DEVICE_NAME_MAX);
    name[HA_LOGICAL_DEVICE_NAME_MAX] = '\0';

    const sys_error_t err =
        device_rename(s_domain, (uint8_t)DOMAIN_SOURCE_UI, device_id, name);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Remove Logical Device (B7): adapter remove + canonical cascade — через Device service. */
static uint16_t web_do_logical_device_remove(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_id_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_id_t device_id = 0;
    memcpy(&device_id, args, sizeof(device_id));
    const sys_error_t err = device_remove(s_domain, (uint8_t)DOMAIN_SOURCE_UI, device_id);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Rename логической Entity по entity_id (A6.1): adapter не участвует. */
static uint16_t web_do_entity_rename(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_entity_id_t) + HA_ENTITY_NAME_MAX) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_entity_rename_t request = {0};
    memcpy(&request.entity, args, sizeof(request.entity));
    memcpy(request.name, args + sizeof(request.entity), HA_ENTITY_NAME_MAX);

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err =
        domain_post(s_domain, HA_CMD_ENTITY_RENAME, &request, sizeof(request), NULL, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Canonical Automation PUT: entity-native правило (ha_rule_t) — без compile. */
static uint16_t web_do_automation_put(const uint8_t *args, size_t len)
{
    ha_automation_key_t key = {0};
    ha_rule_t rule = {0};
    size_t used = 0;
    const uint16_t rc = web_rule_decode(args, len, &key, &rule, &used);
    if (rc != (uint16_t)SYS_CODE_OK) {
        return rc;
    }
    if (used != len) { /* strict: без хвостовых байтов */
        return SYS_CODE_INVALID_SIZE;
    }

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION, &key,
                                              &rule, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* f32 ↔ bits (LE-копия). */
static uint32_t web_float_bits(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/*
 * Entity-native Automation wire (A6.2): u64 id, u8 enabled, u8 kind; trigger по виду
 * (EVENT{u64 entity,u8 event_id} | TIME{u16 minutes,u8 mask} | STATE{u64 entity,u16 property,
 * u8 op,u8 edge,f32 value,f32 value2}); u8 conditions_count; conditions[]{u64 entity,
 * u16 property,u8 op,f32 value,f32 value2}; action{u64 entity,u16 property,u8 action,
 * u8 value_kind[, scalar u8 kind+u32 | xy f32+f32]}. Без uid/endpoint/cluster/attr.
 */
static uint16_t web_rule_decode(const uint8_t *a, size_t len, ha_automation_key_t *key,
                                ha_rule_t *out, size_t *used)
{
    size_t o = 0;
#define NEED(n)                          \
    do {                                 \
        if (o + (size_t)(n) > len) {     \
            return SYS_CODE_INVALID_SIZE; \
        }                                \
    } while (0)

    NEED(10);
    key->id = get_u64(a + o);
    o += 8;
    out->enabled = a[o++];
    out->trigger.kind = a[o++];
    switch ((ha_automation_trigger_kind_t)out->trigger.kind) {
    case HA_TRIGGER_DEVICE_EVENT:
        NEED(9);
        out->trigger.entity = get_u64(a + o);
        o += 8;
        out->trigger.event_id = (ha_event_id_t)a[o++];
        break;
    case HA_TRIGGER_TIME:
        NEED(3);
        out->trigger.minutes_of_day = get_u16(a + o);
        o += 2;
        out->trigger.weekday_mask = a[o++];
        break;
    case HA_TRIGGER_STATE: {
        NEED(18);
        out->trigger.entity = get_u64(a + o);
        o += 8;
        out->trigger.property = (ha_property_id_t)get_u16(a + o);
        o += 2;
        out->trigger.op = a[o++];
        out->trigger.edge = a[o++];
        const uint32_t bv = get_u32(a + o);
        o += 4;
        const uint32_t b2 = get_u32(a + o);
        o += 4;
        memcpy(&out->trigger.value, &bv, sizeof(bv));
        memcpy(&out->trigger.value2, &b2, sizeof(b2));
        break;
    }
    default:
        return SYS_CODE_INVALID_ARG;
    }

    NEED(1);
    out->conditions_count = a[o++];
    if (out->conditions_count > HA_RULE_CONDITIONS_MAX) {
        return SYS_CODE_INVALID_ARG;
    }
    for (uint8_t i = 0; i < out->conditions_count; i++) {
        NEED(19);
        ha_rule_condition_t *c = &out->conditions[i];
        c->ref.entity = get_u64(a + o);
        o += 8;
        c->ref.property = (ha_property_id_t)get_u16(a + o);
        o += 2;
        c->cmp.op = a[o++];
        const uint32_t bv = get_u32(a + o);
        o += 4;
        const uint32_t b2 = get_u32(a + o);
        o += 4;
        memcpy(&c->cmp.value, &bv, sizeof(bv));
        memcpy(&c->cmp.value2, &b2, sizeof(b2));
    }

    NEED(13);
    out->action_ref.entity = get_u64(a + o);
    o += 8;
    out->action_ref.property = (ha_property_id_t)get_u16(a + o);
    o += 2;
    out->action = a[o++];
    out->action_value.kind = (ha_command_value_kind_t)a[o++];
    if (out->action_value.kind == HA_COMMAND_VALUE_SCALAR) {
        NEED(5);
        const ha_value_kind_t kind = (ha_value_kind_t)a[o++];
        const uint32_t bits = get_u32(a + o);
        o += 4;
        if (!bits_to_value(kind, bits, &out->action_value.value.scalar)) {
            return SYS_CODE_INVALID_ARG;
        }
    } else if (out->action_value.kind == HA_COMMAND_VALUE_XY) {
        NEED(8);
        const uint32_t bx = get_u32(a + o);
        o += 4;
        const uint32_t by = get_u32(a + o);
        o += 4;
        float x = 0.0f;
        float y = 0.0f;
        memcpy(&x, &bx, sizeof(x));
        memcpy(&y, &by, sizeof(y));
        if (!isfinite(x) || !isfinite(y)) {
            return SYS_CODE_INVALID_ARG;
        }
        out->action_value.value.xy.x = x;
        out->action_value.value.xy.y = y;
    } else if (out->action_value.kind != HA_COMMAND_VALUE_NONE) {
        return SYS_CODE_INVALID_ARG;
    }
    *used = o;
    return SYS_CODE_OK;
#undef NEED
}

static uint16_t web_rule_encode(const ha_automation_key_t *key, const ha_rule_t *rule, uint8_t *out,
                                uint16_t *out_len)
{
    size_t o = 0;
    put_u64(out + o, key->id);
    o += 8;
    out[o++] = rule->enabled;
    out[o++] = rule->trigger.kind;
    switch ((ha_automation_trigger_kind_t)rule->trigger.kind) {
    case HA_TRIGGER_DEVICE_EVENT:
        put_u64(out + o, rule->trigger.entity);
        o += 8;
        out[o++] = (uint8_t)rule->trigger.event_id;
        break;
    case HA_TRIGGER_TIME:
        put_u16(out + o, rule->trigger.minutes_of_day);
        o += 2;
        out[o++] = rule->trigger.weekday_mask;
        break;
    case HA_TRIGGER_STATE:
        put_u64(out + o, rule->trigger.entity);
        o += 8;
        put_u16(out + o, (uint16_t)rule->trigger.property);
        o += 2;
        out[o++] = rule->trigger.op;
        out[o++] = rule->trigger.edge;
        put_u32(out + o, web_float_bits(rule->trigger.value));
        o += 4;
        put_u32(out + o, web_float_bits(rule->trigger.value2));
        o += 4;
        break;
    default:
        return SYS_CODE_INVALID_ARG;
    }
    out[o++] = rule->conditions_count;
    for (uint8_t i = 0; i < rule->conditions_count && i < HA_RULE_CONDITIONS_MAX; i++) {
        const ha_rule_condition_t *c = &rule->conditions[i];
        put_u64(out + o, c->ref.entity);
        o += 8;
        put_u16(out + o, (uint16_t)c->ref.property);
        o += 2;
        out[o++] = c->cmp.op;
        put_u32(out + o, web_float_bits(c->cmp.value));
        o += 4;
        put_u32(out + o, web_float_bits(c->cmp.value2));
        o += 4;
    }
    put_u64(out + o, rule->action_ref.entity);
    o += 8;
    put_u16(out + o, (uint16_t)rule->action_ref.property);
    o += 2;
    out[o++] = rule->action;
    out[o++] = (uint8_t)rule->action_value.kind;
    if (rule->action_value.kind == HA_COMMAND_VALUE_SCALAR) {
        out[o++] = (uint8_t)rule->action_value.value.scalar.kind;
        put_u32(out + o, value_to_bits(&rule->action_value.value.scalar));
        o += 4;
    } else if (rule->action_value.kind == HA_COMMAND_VALUE_XY) {
        put_u32(out + o, web_float_bits(rule->action_value.value.xy.x));
        o += 4;
        put_u32(out + o, web_float_bits(rule->action_value.value.xy.y));
        o += 4;
    }
    *out_len = (uint16_t)o;
    return SYS_CODE_OK;
}

static uint16_t web_do_permit_join(const uint8_t *args, size_t len)
{
    if (len != 1) {
        return SYS_CODE_INVALID_SIZE;
    }
    const uint8_t seconds = args[0];
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err =
        domain_post(s_domain, HA_CMD_PERMIT_JOIN, &seconds, sizeof(seconds), NULL, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_automation_remove(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_automation_key_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_automation_key_t key;
    memcpy(&key, args, sizeof(key));

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err =
        domain_entity_remove(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION, &key, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* --- экраны Display: group / group_item --------------------------------- */

/* Максимум виджетов, снимаемых с экрана за одно удаление (пометки-сироты не растут). */
#define WEB_GROUP_REMOVE_MAX 64

static uint16_t web_do_group_put(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_group_key_t) + sizeof(ha_group_record_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_group_key_t key;
    ha_group_record_t record;
    memcpy(&key, args, sizeof(key));
    memcpy(&record, args + sizeof(key), sizeof(record));
    record.title[HA_GROUP_TITLE_MAX - 1] = '\0';

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err =
        domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_GROUP, &key, &record, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

typedef struct {
    uint64_t group_id;
    size_t count;
    ha_group_item_key_t keys[WEB_GROUP_REMOVE_MAX];
} group_items_ctx_t;

static bool collect_group_items(const void *key, const void *record, void *ctx)
{
    (void)record;
    const ha_group_item_key_t *item = (const ha_group_item_key_t *)key;
    group_items_ctx_t *out = (group_items_ctx_t *)ctx;
    if (item->group_id == out->group_id && out->count < WEB_GROUP_REMOVE_MAX) {
        out->keys[out->count++] = *item;
    }
    return true;
}

static uint16_t web_do_group_remove(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_group_key_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_group_key_t key;
    memcpy(&key, args, sizeof(key));

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;

    /* Виджеты экрана держатся на group_id в ключе: снимаем их вместе с экраном. */
    group_items_ctx_t ctx;
    ctx.group_id = key.id;
    ctx.count = 0;
    (void)domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, collect_group_items,
                             &ctx);
    for (size_t i = 0; i < ctx.count; i++) {
        (void)domain_entity_remove(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &ctx.keys[i],
                                   &meta);
    }

    const sys_error_t err =
        domain_entity_remove(s_domain, (domain_entity_t)HA_ENTITY_GROUP, &key, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Canonical group item: браузер шлёт (group_id, entity, property) — ключ уже entity-native. */
static uint16_t web_do_group_item_put(const uint8_t *a, size_t len)
{
    if (len != 52) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_group_item_key_t key = {0};
    key.group_id = get_u64(a);
    key.state.entity = get_u64(a + 8);
    key.state.property = (ha_property_id_t)get_u16(a + 16);
    ha_group_item_record_t record = {0};
    record.order = get_u16(a + 18);
    memcpy(record.title, a + 20, HA_GROUP_ITEM_TITLE_MAX);
    record.title[HA_GROUP_ITEM_TITLE_MAX - 1] = '\0';

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &key,
                                              &record, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_group_item_remove(const uint8_t *a, size_t len)
{
    if (len != 18) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_group_item_key_t key = {0};
    key.group_id = get_u64(a);
    key.state.entity = get_u64(a + 8);
    key.state.property = (ha_property_id_t)get_u16(a + 16);

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err =
        domain_entity_remove(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &key, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Локация/пояс (ручное место или пояс): запись сущности LOCATION системы. */
static uint16_t web_do_location_put(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_id_t) + sizeof(ha_location_record_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_id_t uid;
    ha_location_record_t record;
    memcpy(&uid, args, sizeof(uid));
    memcpy(&record, args + sizeof(uid), sizeof(record));
    record.name[HA_LOCATION_NAME_MAX - 1] = '\0';

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_LOCATION, &uid,
                                              &record, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static void web_request_snapshot(int fd)
{
    const web_inbox_item_t item = {.kind = 1, .fd = fd};
    xQueueSend(s_inbox, &item, 0);
}

/* Команды fire-and-forget: ответа нет, UI реагирует только на реальное состояние. */
static void web_handle_command(int fd, uint8_t cmd, const uint8_t *args, size_t len)
{
    if (cmd == (uint8_t)WEB_CMD_SNAPSHOT) {
        web_request_snapshot(fd);
        return; /* клиент получит SYNC_BEGIN… */
    }

    uint16_t status = (uint16_t)SYS_CODE_INVALID_ARG;
    switch (cmd) {
    case WEB_CMD_ENTITY_RENAME:
        status = web_do_entity_rename(args, len);
        break;
    case WEB_CMD_DEVICE_RENAME:
        status = web_do_device_rename(args, len);
        break;
    case WEB_CMD_LOGICAL_DEVICE_REMOVE:
        status = web_do_logical_device_remove(args, len);
        break;
    case WEB_CMD_PROPERTY_COMMAND:
        status = web_do_property_command(args, len);
        break;
    case WEB_CMD_AUTOMATION_PUT:
        status = web_do_automation_put(args, len);
        break;
    case WEB_CMD_AUTOMATION_REMOVE:
        status = web_do_automation_remove(args, len);
        break;
    case WEB_CMD_PERMIT_JOIN:
        status = web_do_permit_join(args, len);
        break;
    case WEB_CMD_GROUP_PUT:
        status = web_do_group_put(args, len);
        break;
    case WEB_CMD_GROUP_REMOVE:
        status = web_do_group_remove(args, len);
        break;

    case WEB_CMD_GROUP_ITEM_PUT:
        status = web_do_group_item_put(args, len);
        break;
    case WEB_CMD_GROUP_ITEM_REMOVE:
        status = web_do_group_item_remove(args, len);
        break;
    case WEB_CMD_LOCATION_PUT:
        status = web_do_location_put(args, len);
        break;
    default:
        break;
    }
    if (status != (uint16_t)SYS_CODE_OK) {
        ESP_LOGW(TAG, "command %u rejected: code=%u", (unsigned)cmd, (unsigned)status);
    }
}

/* --- HTTP / WebSocket --------------------------------------------------- */

#ifdef WEB_UI_EMBEDDED
/* Собранный web-ui встроен в прошивку (web/ui). index.html — как есть, скрипты/стили — gzip. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz_start[] asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[] asm("_binary_app_css_gz_end");

static esp_err_t send_embedded(httpd_req_t *req, const char *content_type, const uint8_t *begin,
                               const uint8_t *end, bool gzip)
{
    httpd_resp_set_type(req, content_type);
    /* Имена файлов фиксированные: запрещаем кэш, иначе браузер держит старую сборку. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (gzip) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    return httpd_resp_send(req, (const char *)begin, (ssize_t)(end - begin));
}

static esp_err_t root_handler(httpd_req_t *req)
{
    return send_embedded(req, "text/html; charset=utf-8", index_html_start, index_html_end, false);
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    return send_embedded(req, "application/javascript; charset=utf-8", app_js_gz_start,
                         app_js_gz_end, true);
}

static esp_err_t app_css_handler(httpd_req_t *req)
{
    return send_embedded(req, "text/css; charset=utf-8", app_css_gz_start, app_css_gz_end, true);
}
#else
static const char *ROOT_PAGE =
    "<!doctype html><meta charset=utf-8><title>ESP32-P4 HA</title>"
    "<h1>ESP32-P4 Home Automation</h1><p>Web service is up (no embedded UI).</p>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, ROOT_PAGE, HTTPD_RESP_USE_STRLEN);
}
#endif

static esp_err_t ws_handler(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        /* Snapshot запрашивает клиент командой SNAPSHOT: слать из GET нельзя —
         * ответ 101 завершается после возврата хендлера, и кадр уйдёт раньше. */
        ESP_LOGI(TAG, "WS client connected (fd=%d)", fd);
        return ESP_OK;
    }

    uint8_t buf[WEB_CMD_BUF];
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (frame.len > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        return err;
    }

    web_hdr_t hdr;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (frame.len >= WEB_PROTO_HDR_SIZE &&
        web_frame_decode(frame.payload, frame.len, &hdr, &payload, &payload_len) &&
        hdr.type == (uint8_t)WEB_MSG_COMMAND && payload_len >= 1) {
        web_handle_command(fd, payload[0], payload + 1, payload_len - 1);
    }
    return ESP_OK;
}

static sys_error_t server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Верхняя граница задана LWIP: 3 сокета держит сам HTTP-сервер (дефолт 7). */
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_IO);
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    const httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &root);
#ifdef WEB_UI_EMBEDDED
    const httpd_uri_t app_js = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = app_js_handler,
    };
    const httpd_uri_t app_css = {
        .uri = "/app.css",
        .method = HTTP_GET,
        .handler = app_css_handler,
    };
    httpd_register_uri_handler(s_server, &app_js);
    httpd_register_uri_handler(s_server, &app_css);
#endif
    httpd_register_uri_handler(s_server, &ws);
    return SYS_OK;
}

/* --- подписка Domain ---------------------------------------------------- */

static bool web_accept(const domain_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_inbox == NULL) {
        return false;
    }
    const web_inbox_item_t item = {.kind = 0, .event = *event};
    return xQueueSend(s_inbox, &item, 0) == pdTRUE;
}

static sys_error_t web_subscribe(void)
{
    s_inbox = xQueueCreate(WEB_INBOX_LENGTH, sizeof(web_inbox_item_t));
    if (s_inbox == NULL) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_NO_MEM);
    }

    domain_subscription_desc_t desc = {0};
    desc.kind_mask = (1u << (uint32_t)DOMAIN_FACT_ENTITY_UPSERTED) |
                     (1u << (uint32_t)DOMAIN_FACT_ENTITY_REMOVED) |
                     (1u << (uint32_t)DOMAIN_FACT_EVENT);
    desc.source_mask = 0; /* любые источники */
    desc.entity = 0;      /* любые типы */
    desc.try_push = web_accept;
    desc.ctx = NULL;

    domain_subscription_t *sub = NULL;
    return domain_subscribe(s_domain, &desc, &sub);
}

/* --- задача ------------------------------------------------------------- */

static void web_task(void *arg)
{
    s_domain = (domain_t *)arg;

    if (sys_failed(server_start())) {
        ESP_LOGE(TAG, "HTTP server not started");
        vTaskDelete(NULL);
        return;
    }
    if (sys_failed(web_subscribe())) {
        ESP_LOGE(TAG, "domain subscribe failed; deltas disabled");
    }
    ESP_LOGI(TAG, "web server started");

    for (;;) {
        web_inbox_item_t item = {0};
        if (xQueueReceive(s_inbox, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.kind == 0) {
            web_send_fact(&item.event);
        } else {
            web_send_snapshot(item.fd);
        }
    }
}

sys_error_t web_start(domain_t *domain)
{
    if (domain == NULL) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_INVALID_ARG);
    }
    if (xTaskCreate(web_task, "web", WEB_TASK_STACK, domain, WEB_TASK_PRIORITY, NULL) != pdPASS) {
        return sys_error_make(SYS_LAYER_WEB, SYS_CODE_NO_MEM);
    }
    return SYS_OK;
}
