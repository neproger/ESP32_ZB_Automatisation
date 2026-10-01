#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "domain/domain.h"
#include "esp_log.h"

/*
 * Вертикальный срез Domain на железе: RAM-сущность, доставка факта подписчику и
 * переоткрытие FLASH-сущности. Это smoke-проверка связки Domain + mstore на целевом
 * железе, а не тест Domain: поведение проверяется host-тестами (domain/tests).
 */
static const char *TAG = "domain_test";

typedef struct {
    uint32_t id;
} sensor_key_t;

typedef struct {
    int32_t value;
} sensor_state_t;

#define TYPE_SENSOR 1
#define CMD_SET_LEVEL 1

static void require(bool condition, const char *what)
{
    if (!condition) {
        ESP_LOGE(TAG, "FAIL: %s", what);
        abort();
    }
}

static void require_ok(sys_error_t err, const char *what)
{
    if (sys_failed(err)) {
        ESP_LOGE(TAG, "FAIL: %s layer=%u code=%u", what, (unsigned)err.layer,
                 (unsigned)err.code);
        abort();
    }
}

static const char *fact_kind_name(uint8_t kind)
{
    switch (kind) {
    case DOMAIN_FACT_ENTITY_UPSERTED: return "ENTITY_UPSERTED";
    case DOMAIN_FACT_ENTITY_REMOVED: return "ENTITY_REMOVED";
    case DOMAIN_FACT_EVENT: return "EVENT";
    case DOMAIN_FACT_COMMAND_SENT: return "COMMAND_SENT";
    case DOMAIN_FACT_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

static const char *fact_op_name(uint8_t op)
{
    switch (op) {
    case DOMAIN_OP_ENTITY_PUT: return "ENTITY_PUT";
    case DOMAIN_OP_ENTITY_REMOVE: return "ENTITY_REMOVE";
    case DOMAIN_OP_COMMAND: return "COMMAND";
    case DOMAIN_OP_PAYLOAD_PUT: return "PAYLOAD_PUT";
    case DOMAIN_OP_ENTITY_GET: return "ENTITY_GET";
    case DOMAIN_OP_ENTITY_ITER: return "ENTITY_ITER";
    default: return "UNKNOWN";
    }
}

static const char *fact_source_name(uint8_t source)
{
    switch (source) {
    case DOMAIN_SOURCE_ZIGBEE: return "ZIGBEE";
    case DOMAIN_SOURCE_UI: return "UI";
    case DOMAIN_SOURCE_AUTOMATION: return "AUTOMATION";
    case DOMAIN_SOURCE_SYSTEM: return "SYSTEM";
    default: return "UNKNOWN";
    }
}

static void fact_value_text(const domain_value_t *value, char *out, size_t size)
{
    switch (value->type) {
    case DOMAIN_VALUE_BOOL: snprintf(out, size, "bool=%u", (unsigned)value->v.u32); break;
    case DOMAIN_VALUE_I32: snprintf(out, size, "i32=%d", (int)value->v.i32); break;
    case DOMAIN_VALUE_U32: snprintf(out, size, "u32=%lu", (unsigned long)value->v.u32); break;
    case DOMAIN_VALUE_F32: snprintf(out, size, "f32=%.3f", (double)value->v.f32); break;
    case DOMAIN_VALUE_ENUM: snprintf(out, size, "enum=%lu", (unsigned long)value->v.u32); break;
    default: snprintf(out, size, "value=-"); break;
    }
}

/*
 * Журнал в консоль: подписчик печатает каждую запись, доставленную Dispatcher'ом.
 * Журнал читает только Dispatcher, прямого чтения ring'а нет (JOURNAL.md §5).
 */
static bool journal_log(const domain_event_t *event, void *ctx)
{
    size_t *delivered = (size_t *)ctx;
    (*delivered)++;

    char key_text[DOMAIN_EVENT_KEY_MAX * 3 + 1] = {0};
    for (uint8_t i = 0; i < event->key_size && i < DOMAIN_EVENT_KEY_MAX; i++) {
        snprintf(&key_text[i * 3], 4, "%02x ", event->key[i]);
    }

    char value_text[24] = {0};
    fact_value_text(&event->value, value_text, sizeof(value_text));

    char tail_text[32] = {0};
    if (event->kind == (uint8_t)DOMAIN_FACT_ERROR) {
        snprintf(tail_text, sizeof(tail_text), "err layer=%u code=%u",
                 (unsigned)event->error.layer, (unsigned)event->error.code);
    } else if (event->payload_ref != 0) {
        snprintf(tail_text, sizeof(tail_text), "payload=%u", (unsigned)event->payload_ref);
    }

    ESP_LOGI(TAG, "#%llu ts=%llums %s op=%s src=%s entity=%lu key=[%s] %s %s",
             (unsigned long long)event->event_id, (unsigned long long)event->ts,
             fact_kind_name(event->kind), fact_op_name(event->op), fact_source_name(event->source),
             (unsigned long)event->entity, key_text, value_text, tail_text);
    return true;
}

static const domain_entity_desc_t ram_desc = {
    .type = TYPE_SENSOR,
    .key_size = sizeof(sensor_key_t),
    .payload_size = sizeof(sensor_state_t),
    .capacity = 2,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

static void entity_store_slice(domain_t *domain)
{
    const sensor_key_t key = {.id = 7};
    const sensor_state_t written = {.value = 25};
    sensor_state_t read_back = {0};
    bool changed = false;

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    meta.value.type = (uint8_t)DOMAIN_VALUE_I32;
    meta.value.v.i32 = written.value;

    require_ok(domain_entity_put(domain, TYPE_SENSOR, &key, &written, &meta, &changed),
               "first put");
    require(changed, "first put changed state");

    /* Повтор того же значения — не изменение: факта нет, changed = false. */
    require_ok(domain_entity_put(domain, TYPE_SENSOR, &key, &written, &meta, &changed),
               "repeated put");
    require(!changed, "repeated put is not a change");

    require_ok(domain_entity_get(domain, TYPE_SENSOR, &key, &read_back), "get");
    require(read_back.value == written.value, "read back written value");

    /* Исчерпание ёмкости таблицы: ошибка хранилища доходит без перекодирования. */
    const sensor_key_t second = {.id = 8};
    const sensor_key_t third = {.id = 9};
    require_ok(domain_entity_put(domain, TYPE_SENSOR, &second, &written, &meta, &changed),
               "second put");
    sys_error_t err = domain_entity_put(domain, TYPE_SENSOR, &third, &written, &meta, &changed);
    require(sys_is(err, SYS_CODE_NO_SPACE), "third put hits capacity");
    ESP_LOGI(TAG, "no space error: layer=%u code=%u", (unsigned)err.layer, (unsigned)err.code);

    require_ok(domain_entity_remove(domain, TYPE_SENSOR, &key, &meta), "remove");
    require(sys_is(domain_entity_get(domain, TYPE_SENSOR, &key, &read_back), SYS_CODE_NOT_FOUND),
            "removed key is absent");
}

static sys_error_t set_level_executor(domain_command_t type, const void *args, size_t args_size,
                                      void *ctx)
{
    size_t *calls = (size_t *)ctx;
    (*calls)++;
    const uint8_t level = (args != NULL && args_size >= sizeof(level))
                              ? *(const uint8_t *)args
                              : 0;
    ESP_LOGI(TAG, "executor got command=%u level=%u", (unsigned)type, (unsigned)level);
    return SYS_OK;
}

static void payload_slice(domain_t *domain)
{
    const char *details = "vendor-specific details";
    domain_payload_ref_t ref = 0;
    require_ok(domain_payload_put(domain, NULL, NULL, details, strlen(details) + 1, &ref),
               "payload put");

    char read_back[64] = {0};
    require_ok(domain_payload_get(domain, ref, read_back, strlen(details) + 1), "payload get");
    require(strcmp(read_back, details) == 0, "payload survived the round trip");
}

static void command_slice(domain_t *domain)
{
    size_t calls = 0;
    require_ok(domain_register_command(domain, CMD_SET_LEVEL, set_level_executor, &calls),
               "register command");

    const uint8_t level = 32;

    /* Адресат команды: COMMAND_SENT должен нести entity и key, иначе подписчик не
     * относит команду к устройству (docs/domain/COMMANDS.md §8). */
    const sensor_key_t target_key = {.id = 21};
    const domain_fact_target_t target = {.entity = TYPE_SENSOR, .key = &target_key};
    require_ok(domain_post(domain, CMD_SET_LEVEL, &level, sizeof(level), &target, NULL),
               "post command");
    require(calls == 1, "executor was called once");
}

static void flash_slice(void)
{
    domain_entity_desc_t desc = ram_desc;
    desc.backing = DOMAIN_BACKING_FLASH;
    desc.persist_key = "domain.sensor";

    const sensor_key_t key = {.id = 42};
    const sensor_state_t written = {.value = 55};
    sensor_state_t read_back = {0};
    bool changed = false;

    domain_t domain = {0};
    require_ok(domain_init(&domain, 2, 16, 8, 64), "domain init (flash writer)");
    require_ok(domain_register_entity(&domain, &desc), "register flash entity");
    require_ok(domain_entity_put(&domain, TYPE_SENSOR, &key, &written, NULL, &changed),
               "flash put");
    require_ok(domain_deinit(&domain), "domain deinit");

    /*
     * Переоткрытие: новая инициализация Domain с тем же persist_key должна увидеть
     * записанное состояние. Это и есть проверка persistence на целевом железе.
     */
    domain_t reopened = {0};
    require_ok(domain_init(&reopened, 2, 16, 8, 64), "domain reopen");
    require_ok(domain_register_entity(&reopened, &desc), "register after reopen");
    require_ok(domain_entity_get(&reopened, TYPE_SENSOR, &key, &read_back), "read after reopen");
    require(read_back.value == written.value, "state survived reopen");
    require_ok(domain_deinit(&reopened), "domain deinit after reopen");
}

void app_main(void)
{
    domain_t domain = {0};
    require_ok(domain_init(&domain, 2, 16, 8, 64), "domain init");
    require_ok(domain_register_entity(&domain, &ram_desc), "register ram entity");

    /* Одна подписка на всё: журнал в консоль, любые факты и источники. */
    size_t received = 0;
    domain_subscription_desc_t desc = {0};
    desc.kind_mask = 0;
    desc.source_mask = 0;
    desc.entity = 0;
    desc.try_push = journal_log;
    desc.wake = NULL;
    desc.ctx = &received;

    domain_subscription_t *journal = NULL;
    require_ok(domain_subscribe(&domain, &desc, &journal), "subscribe journal to console");

    entity_store_slice(&domain);
    payload_slice(&domain);
    command_slice(&domain);

    size_t delivered = 0;
    require_ok(domain_dispatch_once(&domain, &delivered), "dispatch once");
    require(delivered > 0, "facts reached subscriber");
    require(received == delivered, "console journal got every delivered fact");

    require_ok(domain_unsubscribe(&domain, journal), "unsubscribe");
    require_ok(domain_deinit(&domain), "domain deinit");
    flash_slice();

    ESP_LOGI(TAG, "vertical slice OK");
}
