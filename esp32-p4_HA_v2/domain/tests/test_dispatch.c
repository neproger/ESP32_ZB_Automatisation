#include "domain/domain.h"

#include <stdio.h>
#include <string.h>

#include "domain_internal.h"
#include "domain_journal.h"

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

enum { TYPE_A = 1, TYPE_B = 2 };

typedef struct {
    uint32_t id;
    uint8_t endpoint;
} test_key_t;

typedef struct {
    uint32_t value;
    uint32_t reserved;
} test_record_t;

typedef struct {
    domain_event_t events[16];
    size_t count;
    bool accept;
    size_t wakes;
} inbox_t;

static test_key_t key_of(uint32_t id)
{
    test_key_t key = {0};
    key.id = id;
    key.endpoint = 1;
    return key;
}

static bool inbox_push(const domain_event_t *event, void *ctx)
{
    inbox_t *inbox = (inbox_t *)ctx;
    if (!inbox->accept) {
        return false;
    }
    if (inbox->count >= sizeof(inbox->events) / sizeof(inbox->events[0])) {
        return false;
    }
    inbox->events[inbox->count++] = *event;
    return true;
}

static void inbox_wake(void *ctx)
{
    ((inbox_t *)ctx)->wakes++;
}

static domain_subscription_desc_t sub_all(inbox_t *inbox)
{
    domain_subscription_desc_t desc = {0};
    desc.try_push = inbox_push;
    desc.wake = inbox_wake;
    desc.ctx = inbox;
    return desc;
}

static domain_t *start(domain_entity_desc_t *desc_a, domain_entity_desc_t *desc_b)
{
    static domain_t domain;
    memset(&domain, 0, sizeof(domain));
    CHECK(sys_ok(domain_init(&domain, 4, 8, 8, 64)));

    memset(desc_a, 0, sizeof(*desc_a));
    desc_a->type = TYPE_A;
    desc_a->key_size = sizeof(test_key_t);
    desc_a->payload_size = sizeof(test_record_t);
    desc_a->capacity = 4;
    desc_a->backing = DOMAIN_BACKING_RAM;
    CHECK(sys_ok(domain_register_entity(&domain, desc_a)));

    if (desc_b != NULL) {
        memset(desc_b, 0, sizeof(*desc_b));
        desc_b->type = TYPE_B;
        desc_b->key_size = sizeof(test_key_t);
        desc_b->payload_size = sizeof(test_record_t);
        desc_b->capacity = 4;
        desc_b->backing = DOMAIN_BACKING_RAM;
        CHECK(sys_ok(domain_register_entity(&domain, desc_b)));
    }
    return &domain;
}

static void put(domain_t *domain, domain_entity_t type, uint32_t id)
{
    const test_key_t key = key_of(id);
    test_record_t record = {0};
    record.value = id;
    bool changed = false;
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    CHECK(sys_ok(domain_entity_put(domain, type, &key, &record, &meta, &changed)));
}

static void test_delivery(void)
{
    domain_entity_desc_t desc_a;
    domain_t *domain = start(&desc_a, NULL);

    inbox_t inbox = {0};
    inbox.accept = true;
    domain_subscription_desc_t desc = sub_all(&inbox);
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(domain, &desc, &sub)));

    put(domain, TYPE_A, 1);

    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(domain, &delivered)));
    CHECK(delivered == 1);
    CHECK(inbox.count == 1);
    CHECK(inbox.events[0].kind == DOMAIN_FACT_ENTITY_UPSERTED);
    CHECK(inbox.events[0].entity == TYPE_A);
    CHECK(inbox.events[0].event_id == 1);
    CHECK(inbox.wakes == 1);

    /* Повторная доставка без новых фактов — ноль. */
    CHECK(sys_ok(domain_dispatch_once(domain, &delivered)));
    CHECK(delivered == 0);

    CHECK(sys_ok(domain_unsubscribe(domain, sub)));
    put(domain, TYPE_A, 2);
    CHECK(sys_ok(domain_dispatch_once(domain, &delivered)));
    CHECK(delivered == 0);

    CHECK(sys_ok(domain_deinit(domain)));
}

static void test_filter_by_kind(void)
{
    domain_entity_desc_t desc_a;
    domain_t *domain = start(&desc_a, NULL);

    inbox_t errors = {0};
    errors.accept = true;
    domain_subscription_desc_t desc = sub_all(&errors);
    desc.kind_mask = 1u << DOMAIN_FACT_ERROR;
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(domain, &desc, &sub)));

    put(domain, TYPE_A, 1);
    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(domain, &delivered)));
    CHECK(delivered == 0); /* успешный upsert под фильтр не прошёл */

    /* Переполняем таблицу: capacity 4, пятый ключ даёт NO_SPACE → ERROR. */
    for (uint32_t i = 2; i <= 4; ++i) {
        put(domain, TYPE_A, i);
    }
    const test_key_t overflow_key = key_of(5);
    test_record_t overflow_record = {0};
    bool changed = false;
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    CHECK(sys_is(domain_entity_put(domain, TYPE_A, &overflow_key, &overflow_record, &meta, &changed), SYS_CODE_NO_SPACE));
    CHECK(sys_ok(domain_dispatch_once(domain, &delivered)));
    CHECK(errors.count == 1);
    CHECK(errors.events[0].kind == DOMAIN_FACT_ERROR);
    CHECK(sys_is(errors.events[0].error, SYS_CODE_NO_SPACE));

    CHECK(sys_ok(domain_unsubscribe(domain, sub)));
    CHECK(sys_ok(domain_deinit(domain)));
}

static void test_filter_by_entity(void)
{
    domain_entity_desc_t desc_a;
    domain_entity_desc_t desc_b;
    domain_t *domain = start(&desc_a, &desc_b);

    inbox_t inbox = {0};
    inbox.accept = true;
    domain_subscription_desc_t desc = sub_all(&inbox);
    desc.entity = TYPE_B;
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(domain, &desc, &sub)));

    put(domain, TYPE_A, 1);
    put(domain, TYPE_B, 2);

    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(domain, &delivered)));
    CHECK(delivered == 1);
    CHECK(inbox.count == 1);
    CHECK(inbox.events[0].entity == TYPE_B);

    CHECK(sys_ok(domain_unsubscribe(domain, sub)));
    CHECK(sys_ok(domain_deinit(domain)));
}

static void test_inbox_overflow_is_local_loss(void)
{
    domain_entity_desc_t desc_a;
    domain_t *domain = start(&desc_a, NULL);

    inbox_t inbox = {0};
    inbox.accept = false; /* inbox полон */
    domain_subscription_desc_t desc = sub_all(&inbox);
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(domain, &desc, &sub)));

    put(domain, TYPE_A, 1);
    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(domain, &delivered)));
    CHECK(delivered == 0);
    CHECK(inbox.wakes == 0);

    /* Курсор всё равно продвинулся: потеря локальная и не блокирует доставку дальше. */
    inbox.accept = true;
    put(domain, TYPE_A, 2);
    CHECK(sys_ok(domain_dispatch_once(domain, &delivered)));
    CHECK(delivered == 1);
    CHECK(inbox.events[0].event_id == 2);

    CHECK(sys_ok(domain_unsubscribe(domain, sub)));
    CHECK(sys_ok(domain_deinit(domain)));
}

static void test_gap_is_detected(void)
{
    /* Таблица сущностей должна вместить 10 ключей, а Journal — только 8 фактов. */
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 4, 8, 8, 64)));
    domain_entity_desc_t entity_desc = {0};
    entity_desc.type = TYPE_A;
    entity_desc.key_size = sizeof(test_key_t);
    entity_desc.payload_size = sizeof(test_record_t);
    entity_desc.capacity = 32;
    entity_desc.backing = DOMAIN_BACKING_RAM;
    CHECK(sys_ok(domain_register_entity(&domain, &entity_desc)));

    inbox_t inbox = {0};
    inbox.accept = true;
    domain_subscription_desc_t desc = sub_all(&inbox);
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(&domain, &desc, &sub)));

    /* Ёмкость Journal — 8, пишем 10 фактов: первые два вытеснены до доставки. */
    for (uint32_t i = 1; i <= 10; ++i) {
        put(&domain, TYPE_A, i);
    }

    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(&domain, &delivered)));

    domain_state_t *state = domain_state(&domain);
    CHECK(state->gap_count == 1);
    CHECK(delivered == 8);
    CHECK(inbox.events[0].event_id == 3);

    CHECK(sys_ok(domain_unsubscribe(&domain, sub)));
    CHECK(sys_ok(domain_deinit(&domain)));
}

static void test_dispatch_once_without_counter_still_delivers(void)
{
    domain_entity_desc_t desc_a;
    domain_t *domain = start(&desc_a, NULL);

    inbox_t inbox = {0};
    inbox.accept = true;
    domain_subscription_desc_t desc = sub_all(&inbox);
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(domain, &desc, &sub)));

    put(domain, TYPE_A, 1);
    CHECK(sys_ok(domain_dispatch_once(domain, NULL)));
    CHECK(inbox.count == 1);

    CHECK(sys_ok(domain_unsubscribe(domain, sub)));
    CHECK(sys_ok(domain_deinit(domain)));
}

static void test_unsubscribe_is_idempotent_and_rejects_foreign(void)
{
    domain_entity_desc_t desc_a;
    domain_entity_desc_t desc_b;
    domain_t *domain = start(&desc_a, &desc_b);

    inbox_t inbox = {0};
    inbox.accept = true;
    domain_subscription_desc_t desc = sub_all(&inbox);
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(domain, &desc, &sub)));

    CHECK(sys_ok(domain_unsubscribe(domain, sub)));
    /* Повторная отписка не освобождает память дважды. */
    CHECK(sys_is(domain_unsubscribe(domain, sub), SYS_CODE_NOT_FOUND));
    CHECK(sys_is(domain_unsubscribe(domain, NULL), SYS_CODE_INVALID_ARG));

    CHECK(sys_ok(domain_deinit(domain)));
}

static void test_deinit_with_active_subscription(void)
{
    domain_entity_desc_t desc_a;
    domain_t *domain = start(&desc_a, NULL);

    inbox_t inbox = {0};
    inbox.accept = true;
    domain_subscription_desc_t desc = sub_all(&inbox);
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(domain, &desc, &sub)));

    /* deinit освобождает подписки и сигнал; обращаться к sub после этого нельзя. */
    CHECK(sys_ok(domain_deinit(domain)));
}

int main(void)
{
    test_delivery();
    test_filter_by_kind();
    test_filter_by_entity();
    test_inbox_overflow_is_local_loss();
    test_gap_is_detected();
    test_dispatch_once_without_counter_still_delivers();
    test_unsubscribe_is_idempotent_and_rejects_foreign();
    test_deinit_with_active_subscription();

    if (g_failures != 0) {
        printf("test_dispatch: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_dispatch: OK\n");
    return 0;
}
