#include <stdbool.h>
#include <stdint.h>
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

static bool inbox_push(const domain_event_t *event, void *ctx)
{
    size_t *delivered = (size_t *)ctx;
    (*delivered)++;
    ESP_LOGI(TAG, "fact kind=%u op=%u entity=%u", (unsigned)event->kind, (unsigned)event->op,
             (unsigned)event->entity);
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

static void delivery_slice(domain_t *domain)
{
    size_t received = 0;
    domain_subscription_desc_t desc = {0};
    desc.kind_mask = 0; /* любые факты */
    desc.source_mask = 0;
    desc.entity = 0;
    desc.try_push = inbox_push;
    desc.wake = NULL;
    desc.ctx = &received;

    domain_subscription_t *sub = NULL;
    require_ok(domain_subscribe(domain, &desc, &sub), "subscribe");

    const sensor_key_t key = {.id = 11};
    const sensor_state_t written = {.value = 30};
    bool changed = false;
    require_ok(domain_entity_put(domain, TYPE_SENSOR, &key, &written, NULL, &changed),
               "put for delivery");

    size_t delivered = 0;
    require_ok(domain_dispatch_once(domain, &delivered), "dispatch once");
    require(delivered > 0, "fact reached subscriber");
    require(received == delivered, "inbox got every delivered fact");

    require_ok(domain_unsubscribe(domain, sub), "unsubscribe");
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
    size_t received = 0;
    domain_subscription_desc_t desc = {0};
    desc.try_push = inbox_push;
    desc.ctx = &received;
    domain_subscription_t *sub = NULL;
    require_ok(domain_subscribe(domain, &desc, &sub), "subscribe for payload");

    const char *details = "vendor-specific details";
    domain_payload_ref_t ref = 0;
    require_ok(domain_payload_put(domain, NULL, details, strlen(details) + 1, &ref),
               "payload put");

    size_t delivered = 0;
    require_ok(domain_dispatch_once(domain, &delivered), "dispatch payload event");
    require(received > 0, "payload event reached subscriber");

    char read_back[64] = {0};
    require_ok(domain_payload_get(domain, ref, read_back, strlen(details) + 1), "payload get");
    require(strcmp(read_back, details) == 0, "payload survived the round trip");

    require_ok(domain_unsubscribe(domain, sub), "unsubscribe after payload");
}

static void command_slice(domain_t *domain)
{
    size_t received = 0;
    domain_subscription_desc_t desc = {0};
    desc.try_push = inbox_push;
    desc.ctx = &received;
    domain_subscription_t *sub = NULL;
    require_ok(domain_subscribe(domain, &desc, &sub), "subscribe for command");

    size_t calls = 0;
    require_ok(domain_register_command(domain, CMD_SET_LEVEL, set_level_executor, &calls),
               "register command");

    const uint8_t level = 32;
    require_ok(domain_post(domain, CMD_SET_LEVEL, &level, sizeof(level), NULL), "post command");
    require(calls == 1, "executor was called once");

    size_t delivered = 0;
    require_ok(domain_dispatch_once(domain, &delivered), "dispatch command fact");
    require(received > 0, "COMMAND_SENT reached subscriber");

    require_ok(domain_unsubscribe(domain, sub), "unsubscribe after command");
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

    entity_store_slice(&domain);
    delivery_slice(&domain);
    payload_slice(&domain);
    command_slice(&domain);

    require_ok(domain_deinit(&domain), "domain deinit");
    flash_slice();

    ESP_LOGI(TAG, "vertical slice OK");
}
