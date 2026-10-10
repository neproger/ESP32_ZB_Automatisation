#include "web/web.h"

#include <math.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ha_model/ha_automation.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_settings.h"
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

_Static_assert(WEB_ENTITY_DEVICE == (uint32_t)HA_ENTITY_DEVICE, "entity id: device");
_Static_assert(WEB_ENTITY_STATE == (uint32_t)HA_ENTITY_STATE, "entity id: state");
_Static_assert(WEB_ENTITY_ENDPOINT == (uint32_t)HA_ENTITY_ENDPOINT, "entity id: endpoint");
_Static_assert(WEB_ENTITY_AUTOMATION == (uint32_t)HA_ENTITY_AUTOMATION, "entity id: automation");
_Static_assert(WEB_ENTITY_DEVICE_REMOVE == (uint32_t)HA_ENTITY_DEVICE_REMOVE,
               "entity id: device-remove");
_Static_assert(WEB_ENTITY_LOCATION == (uint32_t)HA_ENTITY_LOCATION, "entity id: location");
_Static_assert(WEB_ENTITY_GROUP == (uint32_t)HA_ENTITY_GROUP, "entity id: group");
_Static_assert(WEB_ENTITY_GROUP_ITEM == (uint32_t)HA_ENTITY_GROUP_ITEM, "entity id: group-item");
_Static_assert(WEB_ENTITY_WEATHER == (uint32_t)HA_ENTITY_WEATHER, "entity id: weather");
_Static_assert(WEB_ENTITY_WIFI_STATUS == (uint32_t)HA_ENTITY_WIFI_STATUS, "entity id: wifi-status");
_Static_assert(WEB_ENTITY_SETTINGS == (uint32_t)HA_ENTITY_SETTINGS, "entity id: settings");

/* Фиксируем layout провода: эти размеры зеркалит web-ui/src/schema.js. */
_Static_assert(sizeof(ha_zb_command_t) == 32, "zb command layout: update web-ui");
_Static_assert(sizeof(ha_automation_record_t) == 144, "automation record layout: update web-ui");

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
    X(WEB_ENTITY_DEVICE, sizeof(ha_device_uid_t), sizeof(ha_device_record_t))               \
    X(WEB_ENTITY_STATE, sizeof(ha_zb_state_key_t), sizeof(ha_zb_state_record_t))            \
    X(WEB_ENTITY_ENDPOINT, sizeof(ha_endpoint_key_t), sizeof(ha_endpoint_record_t))         \
    X(WEB_ENTITY_AUTOMATION, sizeof(ha_automation_key_t), sizeof(ha_automation_record_t))   \
    X(WEB_ENTITY_DEVICE_REMOVE, sizeof(ha_device_uid_t), sizeof(ha_device_remove_record_t)) \
    X(WEB_ENTITY_LOCATION, sizeof(ha_device_uid_t), sizeof(ha_location_record_t))           \
    X(WEB_ENTITY_GROUP, sizeof(ha_group_key_t), sizeof(ha_group_record_t))                  \
    X(WEB_ENTITY_GROUP_ITEM, sizeof(ha_group_item_key_t), sizeof(ha_group_item_record_t))   \
    X(WEB_ENTITY_WEATHER, sizeof(ha_device_uid_t), sizeof(ha_weather_record_t))             \
    X(WEB_ENTITY_WIFI_STATUS, sizeof(ha_device_uid_t), sizeof(ha_wifi_status_record_t))     \
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
 * command ingress и semantic state). bits трактуется по kind. */
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

/* Общий wire-кодек semantic-правила и проекция (определены ниже). */
static size_t web_encode_sem_rule(const ha_sem_rule_t *rule, uint8_t *out);
static bool semantic_automation_payload(const ha_automation_key_t *key,
                                        const ha_automation_record_t *record,
                                        const ha_device_record_t *device_ptr, uint8_t *out,
                                        uint16_t *out_len);

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

/* --- семантическое состояние (Фаза 5.1) --------------------------------- */

/* raw STATE → semantic DTO; false при UNKNOWN/ошибке декода (ложное не шлём). */
static bool semantic_state_payload(const ha_zb_state_key_t *key,
                                   const ha_zb_state_record_t *record, uint8_t *out,
                                   uint16_t *out_len)
{
    const ha_property_id_t property = semantics_property_from_key(key);
    if (property == HA_PROPERTY_UNKNOWN) {
        return false;
    }
    ha_value_t value = {0};
    if (!semantics_state_value(key, record, &value)) {
        return false;
    }
    put_u64(out, key->device_uid);
    out[8] = key->endpoint;
    out[9] = (uint8_t)(property & 0xff);
    out[10] = (uint8_t)(property >> 8);
    out[11] = (uint8_t)value.kind;
    put_u32(out + 12, value_to_bits(&value));
    *out_len = 16;
    return true;
}

static bool semantic_state_remove_payload(const ha_zb_state_key_t *key, uint8_t *out,
                                          uint16_t *out_len)
{
    const ha_property_id_t property = semantics_property_from_key(key);
    if (property == HA_PROPERTY_UNKNOWN) {
        return false;
    }
    put_u64(out, key->device_uid);
    out[8] = key->endpoint;
    out[9] = (uint8_t)(property & 0xff);
    out[10] = (uint8_t)(property >> 8);
    *out_len = 11;
    return true;
}

/*
 * raw AUTOMATION → semantic DTO: u64 id + u8 representable (+ rule). Legacy/невыразимое
 * шлём как representable=0 — UI покажет «неподдерживаемое», не выдумывая смысл.
 */
static bool semantic_automation_payload(const ha_automation_key_t *key,
                                        const ha_automation_record_t *record,
                                        const ha_device_record_t *device_ptr, uint8_t *out,
                                        uint16_t *out_len)
{
    put_u64(out, key->id);
    ha_sem_rule_t rule = {0};
    if (!semantics_decompile_automation(record, device_ptr, &rule)) {
        out[8] = 0; /* representable = 0 */
        *out_len = 9;
        return true;
    }
    out[8] = 1;
    const size_t n = web_encode_sem_rule(&rule, out + 9);
    *out_len = (uint16_t)(9 + n);
    return true;
}

/*
 * Endpoint → semantic capabilities: server-кластеры → properties + actions.
 */
static bool capability_payload(const ha_endpoint_key_t *key, const ha_endpoint_record_t *record,
                               uint8_t *out, uint16_t *out_len)
{
    put_u64(out, key->device_uid);
    out[8] = key->endpoint;
    size_t o = 9;
    const size_t count_off = o++;
    uint8_t count = 0;
    for (uint8_t i = 0; i < record->cluster_count && i < HA_ENDPOINT_CLUSTERS_MAX; i++) {
        if (record->clusters[i].role != HA_ZB_ROLE_SERVER) {
            continue;
        }
        ha_capability_t caps[8];
        const size_t n = semantics_cluster_capabilities(record->clusters[i].cluster_id, caps, 8);
        for (size_t j = 0; j < n; j++) {
            if (o + 3 + HA_CAPABILITY_ACTIONS_MAX > WEB_PROTO_MAX_PAYLOAD) {
                goto done;
            }
            out[o++] = (uint8_t)(caps[j].property & 0xff);
            out[o++] = (uint8_t)(caps[j].property >> 8);
            out[o++] = caps[j].action_count;
            for (uint8_t k = 0; k < caps[j].action_count && k < HA_CAPABILITY_ACTIONS_MAX; k++) {
                out[o++] = (uint8_t)caps[j].actions[k];
            }
            count++;
        }
    }
done:
    out[count_off] = count;
    *out_len = (uint16_t)o;
    return true;
}

/* raw group_item → semantic (group_id, uid, ep, property). UNKNOWN property → не публикуем. */
static bool semantic_group_item_payload(const ha_group_item_key_t *key,
                                        const ha_group_item_record_t *record, uint8_t *out,
                                        uint16_t *out_len)
{
    const ha_property_id_t property = semantics_property_from_key(&key->state);
    if (property == HA_PROPERTY_UNKNOWN) {
        return false;
    }
    put_u64(out, key->group_id);
    put_u64(out + 8, key->state.device_uid);
    out[16] = key->state.endpoint;
    put_u16(out + 17, (uint16_t)property);
    put_u16(out + 19, record->order);
    memcpy(out + 21, record->title, HA_GROUP_ITEM_TITLE_MAX);
    *out_len = 21 + HA_GROUP_ITEM_TITLE_MAX;
    return true;
}

static bool semantic_group_item_remove_payload(const ha_group_item_key_t *key, uint8_t *out,
                                               uint16_t *out_len)
{
    const ha_property_id_t property = semantics_property_from_key(&key->state);
    if (property == HA_PROPERTY_UNKNOWN) {
        return false;
    }
    put_u64(out, key->group_id);
    put_u64(out + 8, key->state.device_uid);
    out[16] = key->state.endpoint;
    put_u16(out + 17, (uint16_t)property);
    *out_len = 19;
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

/* Профили устройств для reverse event при snapshot: собраны ВНЕ automation-итерации,
 * т.к. внутри domain_entity_iter нельзя звать domain_entity_get (лок Domain). */
#define WEB_SNAPSHOT_DEVICES_MAX 32
typedef struct {
    ha_device_uid_t uid;
    char model[HA_DEVICE_MODEL_MAX];
} web_device_model_t;

typedef struct {
    web_device_model_t items[WEB_SNAPSHOT_DEVICES_MAX];
    size_t count;
} web_device_cache_t;

static bool snapshot_collect_device(const void *key, const void *record, void *ctx)
{
    web_device_cache_t *cache = (web_device_cache_t *)ctx;
    if (cache->count >= WEB_SNAPSHOT_DEVICES_MAX) {
        return true;
    }
    const ha_device_record_t *rec = (const ha_device_record_t *)record;
    cache->items[cache->count].uid = *(const ha_device_uid_t *)key;
    memcpy(cache->items[cache->count].model, rec->model, HA_DEVICE_MODEL_MAX);
    cache->count++;
    return true;
}

static const char *device_cache_model(const web_device_cache_t *cache, ha_device_uid_t uid)
{
    for (size_t i = 0; i < cache->count; i++) {
        if (cache->items[i].uid == uid) {
            return cache->items[i].model;
        }
    }
    return NULL;
}

typedef struct {
    int fd;
    const web_schema_t *schema;
    uint32_t *count;
    const web_device_cache_t *devices;
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
    if (s->schema->type == WEB_ENTITY_STATE) {
        uint8_t sem[WEB_PROTO_MAX_PAYLOAD];
        uint16_t sem_len = 0;
        if (semantic_state_payload((const ha_zb_state_key_t *)key,
                                   (const ha_zb_state_record_t *)record, sem, &sem_len)) {
            web_send_frame(s->fd, WEB_MSG_SEMANTIC_STATE, 0, sem, sem_len);
        }
    } else if (s->schema->type == WEB_ENTITY_AUTOMATION) {
        uint8_t sem[WEB_PROTO_MAX_PAYLOAD];
        uint16_t sem_len = 0;
        const ha_automation_record_t *arec = (const ha_automation_record_t *)record;
        ha_device_record_t dev = {0};
        const ha_device_record_t *dev_ptr = NULL;
        if (s->devices != NULL &&
            arec->trigger_kind == (uint8_t)HA_TRIGGER_DEVICE_EVENT) {
            const char *model = device_cache_model(s->devices, arec->trigger_b.event.device_uid);
            if (model != NULL) {
                memcpy(dev.model, model, HA_DEVICE_MODEL_MAX);
                dev_ptr = &dev;
            }
        }
        if (semantic_automation_payload((const ha_automation_key_t *)key, arec, dev_ptr, sem,
                                        &sem_len)) {
            web_send_frame(s->fd, WEB_MSG_SEMANTIC_AUTOMATION, 0, sem, sem_len);
        }
    } else if (s->schema->type == WEB_ENTITY_ENDPOINT) {
        uint8_t cap[WEB_PROTO_MAX_PAYLOAD];
        uint16_t cap_len = 0;
        if (capability_payload((const ha_endpoint_key_t *)key,
                               (const ha_endpoint_record_t *)record, cap, &cap_len)) {
            web_send_frame(s->fd, WEB_MSG_SEMANTIC_CAPABILITIES, 0, cap, cap_len);
        }
    } else if (s->schema->type == WEB_ENTITY_GROUP_ITEM) {
        uint8_t gi[WEB_PROTO_MAX_PAYLOAD];
        uint16_t gi_len = 0;
        if (semantic_group_item_payload((const ha_group_item_key_t *)key,
                                        (const ha_group_item_record_t *)record, gi, &gi_len)) {
            web_send_frame(s->fd, WEB_MSG_SEMANTIC_GROUP_ITEM, 0, gi, gi_len);
        }
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

    uint8_t word[4];
    put_u32(word, total);
    web_send_frame(fd, WEB_MSG_SYNC_BEGIN, 0, word, sizeof(word));

    web_device_cache_t devices = {0};
    domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_DEVICE, snapshot_collect_device,
                       &devices);

    uint32_t sent = 0;
    for (size_t i = 0; i < WEB_SCHEMA_COUNT; i++) {
        snapshot_ctx_t ctx = {
            .fd = fd, .schema = &SCHEMA[i], .count = &sent, .devices = &devices};
        domain_entity_iter(s_domain, (domain_entity_t)SCHEMA[i].type, snapshot_emit, &ctx);
    }

    put_u32(word, sent);
    web_send_frame(fd, WEB_MSG_SYNC_END, 0, word, sizeof(word));
}

/* --- delta -------------------------------------------------------------- */

/*
 * semantic EVENT → WEB_MSG_EVENT (уже HA_EVENT_* от источника). Raw ha_zb_event_t
 * (transient payload) наружу НЕ отдаём — он нужен только legacy-shim Automation в прошивке.
 * DTO: u8 source_kind @0, u64 source_uid @1, u8 endpoint @9, u8 event_id @10,
 *      u8 value_kind @11, u32 value_bits @12 (16).
 */
static void web_send_semantic_event(const domain_event_t *event)
{
    if (event->key_size != sizeof(ha_device_uid_t)) {
        return;
    }
    uint8_t out[16] = {0};
    out[0] = event->source;
    uint64_t uid = 0;
    memcpy(&uid, event->key, sizeof(uid));
    put_u64(out + 1, uid);
    out[10] = (event->value.type == (uint8_t)DOMAIN_VALUE_ENUM) ? (uint8_t)event->value.v.u32
                                                                : (uint8_t)HA_EVENT_NONE;
    web_broadcast(WEB_MSG_EVENT, 0, out, sizeof(out));
}

static void web_send_fact(const domain_event_t *event)
{
    if (event->kind == (uint8_t)DOMAIN_FACT_EVENT) {
        web_send_semantic_event(event);
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
        if (schema->type == WEB_ENTITY_STATE && event->key_size == sizeof(ha_zb_state_key_t)) {
            uint8_t sem[WEB_PROTO_MAX_PAYLOAD];
            uint16_t sem_len = 0;
            if (semantic_state_payload((const ha_zb_state_key_t *)event->key,
                                       (const ha_zb_state_record_t *)record, sem, &sem_len)) {
                web_broadcast(WEB_MSG_SEMANTIC_STATE, 0, sem, sem_len);
            }
        } else if (schema->type == WEB_ENTITY_AUTOMATION &&
                   event->key_size == sizeof(ha_automation_key_t)) {
            const ha_automation_record_t *rec = (const ha_automation_record_t *)record;
            ha_device_record_t dev = {0};
            const ha_device_record_t *dev_ptr = NULL;
            if (rec->trigger_kind == (uint8_t)HA_TRIGGER_DEVICE_EVENT &&
                rec->trigger_b.event.device_uid != 0 &&
                sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_DEVICE,
                                         &rec->trigger_b.event.device_uid, &dev))) {
                dev_ptr = &dev;
            }
            uint8_t sem[WEB_PROTO_MAX_PAYLOAD];
            uint16_t sem_len = 0;
            if (semantic_automation_payload((const ha_automation_key_t *)event->key, rec, dev_ptr,
                                            sem, &sem_len)) {
                web_broadcast(WEB_MSG_SEMANTIC_AUTOMATION, 0, sem, sem_len);
            }
        } else if (schema->type == WEB_ENTITY_ENDPOINT &&
                   event->key_size == sizeof(ha_endpoint_key_t)) {
            uint8_t cap[WEB_PROTO_MAX_PAYLOAD];
            uint16_t cap_len = 0;
            if (capability_payload((const ha_endpoint_key_t *)event->key,
                                   (const ha_endpoint_record_t *)record, cap, &cap_len)) {
                web_broadcast(WEB_MSG_SEMANTIC_CAPABILITIES, 0, cap, cap_len);
            }
        } else if (schema->type == WEB_ENTITY_GROUP_ITEM &&
                   event->key_size == sizeof(ha_group_item_key_t)) {
            uint8_t gi[WEB_PROTO_MAX_PAYLOAD];
            uint16_t gi_len = 0;
            if (semantic_group_item_payload((const ha_group_item_key_t *)event->key,
                                            (const ha_group_item_record_t *)record, gi, &gi_len)) {
                web_broadcast(WEB_MSG_SEMANTIC_GROUP_ITEM, 0, gi, gi_len);
            }
        }
    } else if (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED) {
        web_broadcast(WEB_MSG_ENTITY_REMOVE, 0, payload, (uint16_t)(1 + schema->key_size));
        if (schema->type == WEB_ENTITY_STATE && event->key_size == sizeof(ha_zb_state_key_t)) {
            uint8_t sem[WEB_PROTO_MAX_PAYLOAD];
            uint16_t sem_len = 0;
            if (semantic_state_remove_payload((const ha_zb_state_key_t *)event->key, sem,
                                              &sem_len)) {
                web_broadcast(WEB_MSG_SEMANTIC_STATE_REMOVE, 0, sem, sem_len);
            }
        } else if (schema->type == WEB_ENTITY_AUTOMATION &&
                   event->key_size == sizeof(ha_automation_key_t)) {
            web_broadcast(WEB_MSG_SEMANTIC_AUTOMATION_REMOVE, 0, event->key,
                          (uint16_t)sizeof(ha_automation_key_t));
        } else if (schema->type == WEB_ENTITY_ENDPOINT &&
                   event->key_size == sizeof(ha_endpoint_key_t)) {
            web_broadcast(WEB_MSG_SEMANTIC_CAPABILITIES_REMOVE, 0, event->key,
                          (uint16_t)sizeof(ha_endpoint_key_t));
        } else if (schema->type == WEB_ENTITY_GROUP_ITEM &&
                   event->key_size == sizeof(ha_group_item_key_t)) {
            uint8_t gi[WEB_PROTO_MAX_PAYLOAD];
            uint16_t gi_len = 0;
            if (semantic_group_item_remove_payload((const ha_group_item_key_t *)event->key, gi,
                                                   &gi_len)) {
                web_broadcast(WEB_MSG_SEMANTIC_GROUP_ITEM_REMOVE, 0, gi, gi_len);
            }
        }
    }
}

/* --- команды ------------------------------------------------------------ */

static uint16_t web_do_zb_command(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_zb_command_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_zb_command_t command;
    memcpy(&command, args, sizeof(command));
    if (command.args_len > HA_ZB_COMMAND_ARGS_MAX) {
        return SYS_CODE_INVALID_ARG;
    }

    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    meta.value.type = (uint8_t)DOMAIN_VALUE_ENUM;
    meta.value.v.u32 = command.command_id;

    const sys_error_t err =
        domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &target, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/*
 * Семантическая команда: браузер шлёт property/action/value, ZCL-кодировку делает мост.
 * Граница доверия (WS): валидируем размер, форму значения и конечность чисел.
 */
static uint16_t web_do_semantic_command(const uint8_t *args, size_t len)
{
    if (len < 13) {
        return SYS_CODE_INVALID_SIZE;
    }
    const uint64_t device_uid = get_u64(args + 0);
    const uint8_t endpoint = args[8];
    const uint16_t property_id = get_u16(args + 9);
    const uint8_t action_id = args[11];
    const uint8_t value_kind = args[12];

    ha_command_value_t value = {0};
    if (value_kind == (uint8_t)HA_COMMAND_VALUE_NONE) {
        if (len != 13) { /* strict: точный размер каждой формы */
            return SYS_CODE_INVALID_SIZE;
        }
        value.kind = HA_COMMAND_VALUE_NONE;
    } else if (value_kind == (uint8_t)HA_COMMAND_VALUE_SCALAR) {
        if (len != 18) {
            return SYS_CODE_INVALID_SIZE;
        }
        const ha_value_kind_t scalar_kind = (ha_value_kind_t)args[13];
        const uint32_t bits = get_u32(args + 14);
        value.kind = HA_COMMAND_VALUE_SCALAR;
        if (!bits_to_value(scalar_kind, bits, &value.value.scalar)) {
            return SYS_CODE_INVALID_ARG;
        }
    } else if (value_kind == (uint8_t)HA_COMMAND_VALUE_XY) {
        if (len != 21) {
            return SYS_CODE_INVALID_SIZE;
        }
        const uint32_t xb = get_u32(args + 13);
        const uint32_t yb = get_u32(args + 17);
        float x = 0.0f;
        float y = 0.0f;
        memcpy(&x, &xb, sizeof(x));
        memcpy(&y, &yb, sizeof(y));
        if (!isfinite(x) || !isfinite(y)) {
            return SYS_CODE_INVALID_ARG;
        }
        value.kind = HA_COMMAND_VALUE_XY;
        value.value.xy.x = x;
        value.value.xy.y = y;
    } else {
        return SYS_CODE_INVALID_ARG;
    }

    const ha_zb_state_key_t target = {.device_uid = device_uid, .endpoint = endpoint};
    ha_zb_command_t command = {0};
    if (!semantics_build_command(&target, (ha_property_id_t)property_id, (ha_action_id_t)action_id,
                                 &value, &command)) {
        return SYS_CODE_INVALID_ARG; /* unknown property/action или неверная форма значения */
    }

    const domain_fact_target_t fact = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    meta.value.type = (uint8_t)DOMAIN_VALUE_ENUM;
    meta.value.v.u32 = command.command_id;
    const sys_error_t err =
        domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &fact, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_device_rename(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_uid_t) + HA_DEVICE_NAME_MAX) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_uid_t uid;
    memcpy(&uid, args, sizeof(uid));

    ha_device_record_t record = {0};
    sys_error_t err = domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &record);
    if (sys_failed(err)) {
        return err.code;
    }
    memcpy(record.name, args + sizeof(uid), HA_DEVICE_NAME_MAX);
    record.name[HA_DEVICE_NAME_MAX - 1] = '\0';

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &record, &meta,
                            &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_automation_put(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_automation_key_t) + sizeof(ha_automation_record_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_automation_key_t key;
    ha_automation_record_t record;
    memcpy(&key, args, sizeof(key));
    memcpy(&record, args + sizeof(key), sizeof(record));

    if (record.conditions_count > HA_AUTOMATION_CONDITIONS_MAX ||
        record.action_args_len > HA_AUTOMATION_ARGS_MAX) {
        return SYS_CODE_INVALID_ARG;
    }

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION, &key,
                                              &record, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/*
 * Общий wire-кодек semantic-правила (read=write): PUT декодирует, snapshot/delta
 * кодируют. Стабильный LE, без C enum/union/memcpy целых структур.
 */
static uint16_t web_decode_sem_rule(const uint8_t *a, size_t len, size_t *used, ha_sem_rule_t *out)
{
    size_t o = 0;
#define NEED(n)                          \
    do {                                 \
        if (o + (size_t)(n) > len) {     \
            return SYS_CODE_INVALID_SIZE; \
        }                                \
    } while (0)

    NEED(2);
    out->enabled = a[o++];
    out->trigger.kind = a[o++];
    switch (out->trigger.kind) {
    case (uint8_t)HA_TRIGGER_DEVICE_EVENT:
        NEED(9);
        out->trigger.device_uid = get_u64(a + o);
        o += 8;
        out->trigger.event_id = (ha_event_id_t)a[o++];
        break;
    case (uint8_t)HA_TRIGGER_TIME:
        NEED(3);
        out->trigger.minutes_of_day = get_u16(a + o);
        o += 2;
        out->trigger.weekday_mask = a[o++];
        break;
    case (uint8_t)HA_TRIGGER_STATE: {
        NEED(21);
        out->trigger.device_uid = get_u64(a + o);
        o += 8;
        out->trigger.endpoint = a[o++];
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
    if (out->conditions_count > HA_AUTOMATION_CONDITIONS_MAX) {
        return SYS_CODE_INVALID_ARG;
    }
    for (uint8_t i = 0; i < out->conditions_count; i++) {
        NEED(20);
        ha_sem_condition_t *c = &out->conditions[i];
        c->ref.device_uid = get_u64(a + o);
        o += 8;
        c->ref.endpoint = a[o++];
        c->ref.property = (ha_property_id_t)get_u16(a + o);
        o += 2;
        c->op = a[o++];
        const uint32_t bv = get_u32(a + o);
        o += 4;
        const uint32_t b2 = get_u32(a + o);
        o += 4;
        memcpy(&c->value, &bv, sizeof(bv));
        memcpy(&c->value2, &b2, sizeof(b2));
    }

    NEED(13);
    ha_sem_action_t *act = &out->action;
    act->target.device_uid = get_u64(a + o);
    o += 8;
    act->target.endpoint = a[o++];
    act->target.property = (ha_property_id_t)get_u16(a + o);
    o += 2;
    act->action = (ha_action_id_t)a[o++];
    act->value_kind = a[o++];
    if (act->value_kind == (uint8_t)HA_COMMAND_VALUE_SCALAR) {
        NEED(5);
        const ha_value_kind_t kind = (ha_value_kind_t)a[o++];
        const uint32_t bits = get_u32(a + o);
        o += 4;
        if (!bits_to_value(kind, bits, &act->value)) {
            return SYS_CODE_INVALID_ARG;
        }
    } else if (act->value_kind == (uint8_t)HA_COMMAND_VALUE_XY) {
        NEED(8);
        const uint32_t bx = get_u32(a + o);
        o += 4;
        const uint32_t by = get_u32(a + o);
        o += 4;
        memcpy(&act->x, &bx, sizeof(bx));
        memcpy(&act->y, &by, sizeof(by));
        if (!isfinite(act->x) || !isfinite(act->y)) {
            return SYS_CODE_INVALID_ARG;
        }
    } else if (act->value_kind != (uint8_t)HA_COMMAND_VALUE_NONE) {
        return SYS_CODE_INVALID_ARG;
    }
    *used = o;
    return SYS_CODE_OK;
#undef NEED
}

static size_t web_encode_sem_rule(const ha_sem_rule_t *rule, uint8_t *out)
{
    size_t o = 0;
    out[o++] = rule->enabled;
    out[o++] = rule->trigger.kind;
    switch ((ha_automation_trigger_kind_t)rule->trigger.kind) {
    case HA_TRIGGER_DEVICE_EVENT:
        put_u64(out + o, rule->trigger.device_uid);
        o += 8;
        out[o++] = (uint8_t)rule->trigger.event_id;
        break;
    case HA_TRIGGER_TIME:
        out[o++] = (uint8_t)(rule->trigger.minutes_of_day & 0xff);
        out[o++] = (uint8_t)(rule->trigger.minutes_of_day >> 8);
        out[o++] = rule->trigger.weekday_mask;
        break;
    case HA_TRIGGER_STATE: {
        put_u64(out + o, rule->trigger.device_uid);
        o += 8;
        out[o++] = rule->trigger.endpoint;
        out[o++] = (uint8_t)(rule->trigger.property & 0xff);
        out[o++] = (uint8_t)(rule->trigger.property >> 8);
        out[o++] = rule->trigger.op;
        out[o++] = rule->trigger.edge;
        uint32_t bv = 0;
        uint32_t b2 = 0;
        memcpy(&bv, &rule->trigger.value, sizeof(bv));
        memcpy(&b2, &rule->trigger.value2, sizeof(b2));
        put_u32(out + o, bv);
        o += 4;
        put_u32(out + o, b2);
        o += 4;
        break;
    }
    default:
        return 0;
    }
    out[o++] = rule->conditions_count;
    for (uint8_t i = 0; i < rule->conditions_count; i++) {
        const ha_sem_condition_t *c = &rule->conditions[i];
        put_u64(out + o, c->ref.device_uid);
        o += 8;
        out[o++] = c->ref.endpoint;
        out[o++] = (uint8_t)(c->ref.property & 0xff);
        out[o++] = (uint8_t)(c->ref.property >> 8);
        out[o++] = c->op;
        uint32_t bv = 0;
        uint32_t b2 = 0;
        memcpy(&bv, &c->value, sizeof(bv));
        memcpy(&b2, &c->value2, sizeof(b2));
        put_u32(out + o, bv);
        o += 4;
        put_u32(out + o, b2);
        o += 4;
    }
    const ha_sem_action_t *act = &rule->action;
    put_u64(out + o, act->target.device_uid);
    o += 8;
    out[o++] = act->target.endpoint;
    out[o++] = (uint8_t)(act->target.property & 0xff);
    out[o++] = (uint8_t)(act->target.property >> 8);
    out[o++] = (uint8_t)act->action;
    out[o++] = act->value_kind;
    if (act->value_kind == (uint8_t)HA_COMMAND_VALUE_SCALAR) {
        out[o++] = (uint8_t)act->value.kind;
        put_u32(out + o, value_to_bits(&act->value));
        o += 4;
    } else if (act->value_kind == (uint8_t)HA_COMMAND_VALUE_XY) {
        uint32_t bx = 0;
        uint32_t by = 0;
        memcpy(&bx, &act->x, sizeof(bx));
        memcpy(&by, &act->y, sizeof(by));
        put_u32(out + o, bx);
        o += 4;
        put_u32(out + o, by);
        o += 4;
    }
    return o;
}

/*
 * Semantic-правило → physical record: браузер не знает offsets/args/cluster/command id.
 * Компилирует мост; невыразимое правило (неоднозначный event, BETWEEN-триггер) отвергается.
 */
static uint16_t web_do_semantic_automation_put(const uint8_t *a, size_t len)
{
    if (len < 8) {
        return SYS_CODE_INVALID_SIZE;
    }
    const uint64_t id = get_u64(a);
    ha_sem_rule_t rule = {0};
    size_t used = 0;
    const uint16_t rc = web_decode_sem_rule(a + 8, len - 8, &used, &rule);
    if (rc != (uint16_t)SYS_CODE_OK) {
        return rc;
    }
    if (8 + used != len) { /* strict: без хвостовых байтов */
        return SYS_CODE_INVALID_SIZE;
    }

    ha_device_record_t device = {0};
    const ha_device_record_t *device_ptr = NULL;
    if (rule.trigger.device_uid != 0 &&
        sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_DEVICE,
                                 &rule.trigger.device_uid, &device))) {
        device_ptr = &device;
    }

    ha_automation_record_t record = {0};
    if (!semantics_compile_automation(&rule, device_ptr, &record)) {
        return SYS_CODE_INVALID_ARG;
    }

    const ha_automation_key_t key = {.id = id};
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_AUTOMATION, &key,
                                              &record, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_device_remove(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_uid_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_uid_t uid;
    memcpy(&uid, args, sizeof(uid));

    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &uid,
    };
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err =
        domain_post(s_domain, HA_CMD_DEVICE_REMOVE, &uid, sizeof(uid), &target, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
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

static uint16_t web_do_device_remove_cancel(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_uid_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_uid_t uid;
    memcpy(&uid, args, sizeof(uid));

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err = domain_entity_remove(
        s_domain, (domain_entity_t)HA_ENTITY_DEVICE_REMOVE, &uid, &meta);
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

static uint16_t web_do_group_item_put(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_group_item_key_t) + sizeof(ha_group_item_record_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_group_item_key_t key;
    ha_group_item_record_t record;
    memcpy(&key, args, sizeof(key));
    memcpy(&record, args + sizeof(key), sizeof(record));
    record.title[HA_GROUP_ITEM_TITLE_MAX - 1] = '\0';

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &key,
                                              &record, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_group_item_remove(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_group_item_key_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_group_item_key_t key;
    memcpy(&key, args, sizeof(key));

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err =
        domain_entity_remove(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &key, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/*
 * Semantic group item → raw: браузер шлёт (group_id, uid, ep, property), backend восстанавливает
 * physical key через semantics_property_key; persistent layout не меняется.
 */
static uint16_t web_do_semantic_group_item_put(const uint8_t *a, size_t len)
{
    if (len != 53) {
        return SYS_CODE_INVALID_SIZE;
    }
    const uint64_t group_id = get_u64(a);
    const uint64_t uid = get_u64(a + 8);
    const uint8_t ep = a[16];
    const ha_property_id_t property = (ha_property_id_t)get_u16(a + 17);
    const ha_zb_state_key_t context = {.device_uid = uid, .endpoint = ep};
    ha_zb_state_key_t state = {0};
    if (!semantics_property_key(&context, property, &state)) {
        return SYS_CODE_INVALID_ARG;
    }
    ha_group_item_key_t key = {0};
    key.group_id = group_id;
    key.state = state;
    ha_group_item_record_t record = {0};
    record.order = get_u16(a + 19);
    memcpy(record.title, a + 21, HA_GROUP_ITEM_TITLE_MAX);
    record.title[HA_GROUP_ITEM_TITLE_MAX - 1] = '\0';

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    const sys_error_t err = domain_entity_put(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &key,
                                              &record, &meta, &changed);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

static uint16_t web_do_semantic_group_item_remove(const uint8_t *a, size_t len)
{
    if (len != 19) {
        return SYS_CODE_INVALID_SIZE;
    }
    const uint64_t group_id = get_u64(a);
    const uint64_t uid = get_u64(a + 8);
    const uint8_t ep = a[16];
    const ha_property_id_t property = (ha_property_id_t)get_u16(a + 17);
    const ha_zb_state_key_t context = {.device_uid = uid, .endpoint = ep};
    ha_zb_state_key_t state = {0};
    if (!semantics_property_key(&context, property, &state)) {
        return SYS_CODE_INVALID_ARG;
    }
    ha_group_item_key_t key = {0};
    key.group_id = group_id;
    key.state = state;

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    const sys_error_t err =
        domain_entity_remove(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &key, &meta);
    return sys_failed(err) ? err.code : (uint16_t)SYS_CODE_OK;
}

/* Локация/пояс (ручное место или пояс): запись сущности LOCATION системы. */
static uint16_t web_do_location_put(const uint8_t *args, size_t len)
{
    if (len != sizeof(ha_device_uid_t) + sizeof(ha_location_record_t)) {
        return SYS_CODE_INVALID_SIZE;
    }
    ha_device_uid_t uid;
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
    case WEB_CMD_ZB_COMMAND:
        status = web_do_zb_command(args, len);
        break;
    case WEB_CMD_SEMANTIC_COMMAND:
        status = web_do_semantic_command(args, len);
        break;
    case WEB_CMD_SEMANTIC_AUTOMATION_PUT:
        status = web_do_semantic_automation_put(args, len);
        break;
    case WEB_CMD_DEVICE_RENAME:
        status = web_do_device_rename(args, len);
        break;
    case WEB_CMD_AUTOMATION_PUT:
        status = web_do_automation_put(args, len);
        break;
    case WEB_CMD_AUTOMATION_REMOVE:
        status = web_do_automation_remove(args, len);
        break;
    case WEB_CMD_DEVICE_REMOVE:
        status = web_do_device_remove(args, len);
        break;
    case WEB_CMD_DEVICE_REMOVE_CANCEL:
        status = web_do_device_remove_cancel(args, len);
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
    case WEB_CMD_SEMANTIC_GROUP_ITEM_PUT:
        status = web_do_semantic_group_item_put(args, len);
        break;
    case WEB_CMD_SEMANTIC_GROUP_ITEM_REMOVE:
        status = web_do_semantic_group_item_remove(args, len);
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
