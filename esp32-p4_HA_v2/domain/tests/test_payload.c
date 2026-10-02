#include "domain/domain.h"

#include <stdio.h>
#include <string.h>

/*
 * Transient payload: put/get по opaque ref и best-effort семантика вытеснения
 * (docs/domain/TRANSIENT_PAYLOAD.md).
 */

static int failures = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
            failures++;                                                                    \
        }                                                                                  \
    } while (0)

typedef struct {
    uint8_t bytes[32];
} payload_t;

typedef struct {
    bool accept;
    size_t count;
    domain_event_t events[16];
} inbox_t;

static bool inbox_push(const domain_event_t *event, void *ctx)
{
    inbox_t *inbox = (inbox_t *)ctx;
    if (!inbox->accept) {
        return false;
    }
    if (inbox->count < 16) {
        inbox->events[inbox->count] = *event;
    }
    inbox->count++;
    return true;
}

#define CAPACITY 2
#define MAX_SIZE 32

static void put_get(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 1, 8, CAPACITY, MAX_SIZE)));

    const payload_t written = {.bytes = {1, 2, 3, 4}};
    domain_payload_ref_t ref = 0;
    CHECK(sys_ok(domain_payload_put(&domain, NULL, NULL, &written, sizeof(written), &ref)));
    CHECK(ref != 0);

    payload_t read_back = {0};
    CHECK(sys_ok(domain_payload_get(&domain, ref, &read_back, sizeof(read_back))));
    CHECK(memcmp(&read_back, &written, sizeof(written)) == 0);

    /* Неизвестная ссылка — NOT_FOUND, а не мусор. */
    payload_t unused = {0};
    CHECK(sys_is(domain_payload_get(&domain, ref + 1000, &unused, sizeof(unused)),
                 SYS_CODE_NOT_FOUND));

    CHECK(sys_ok(domain_deinit(&domain)));
}

static void evicted_payload_is_stale(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 1, 8, CAPACITY, MAX_SIZE)));

    domain_payload_ref_t first = 0;
    domain_payload_ref_t second = 0;
    domain_payload_ref_t third = 0;
    const payload_t a = {.bytes = {10}};
    const payload_t b = {.bytes = {20}};
    const payload_t c = {.bytes = {30}};

    CHECK(sys_ok(domain_payload_put(&domain, NULL, NULL, &a, sizeof(a), &first)));
    CHECK(sys_ok(domain_payload_put(&domain, NULL, NULL, &b, sizeof(b), &second)));
    /* Ring заполнен: третий вытесняет первый. */
    CHECK(sys_ok(domain_payload_put(&domain, NULL, NULL, &c, sizeof(c), &third)));

    payload_t read_back = {0};
    CHECK(sys_is(domain_payload_get(&domain, first, &read_back, sizeof(read_back)),
                 SYS_CODE_STALE));
    CHECK(sys_ok(domain_payload_get(&domain, third, &read_back, sizeof(read_back))));
    CHECK(read_back.bytes[0] == 30);

    CHECK(sys_ok(domain_deinit(&domain)));
}

static void put_publishes_event_with_ref(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 1, 8, CAPACITY, MAX_SIZE)));

    inbox_t inbox = {0};
    inbox.accept = true;
    domain_subscription_desc_t desc = {0};
    desc.try_push = inbox_push;
    desc.ctx = &inbox;
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(&domain, &desc, &sub)));

    const payload_t written = {.bytes = {7, 7}};
    domain_payload_ref_t ref = 0;
    CHECK(sys_ok(domain_payload_put(&domain, NULL, NULL, &written, sizeof(written), &ref)));

    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(&domain, &delivered)));
    CHECK(delivered == 1);
    CHECK(inbox.count == 1);
    CHECK(inbox.events[0].kind == DOMAIN_FACT_EVENT);
    CHECK(inbox.events[0].op == DOMAIN_OP_PAYLOAD_PUT);
    CHECK(inbox.events[0].payload_ref == ref);
    /* Размер тела едет в самом факте: подписчику не нужен отдельный запрос. */
    CHECK(inbox.events[0].payload_size == sizeof(written));

    CHECK(sys_ok(domain_unsubscribe(&domain, sub)));
    CHECK(sys_ok(domain_deinit(&domain)));
}

static void size_mismatch_is_rejected(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 1, 8, CAPACITY, MAX_SIZE)));

    const payload_t written = {.bytes = {5}};
    domain_payload_ref_t ref = 0;
    CHECK(sys_ok(domain_payload_put(&domain, NULL, NULL, &written, sizeof(written), &ref)));

    uint8_t small[4] = {0};
    CHECK(sys_is(domain_payload_get(&domain, ref, small, sizeof(small)), SYS_CODE_INVALID_SIZE));

    /* Шире максимума типа payload не принимается. */
    uint8_t huge[64] = {0};
    domain_payload_ref_t ignored = 0;
    CHECK(sys_is(domain_payload_put(&domain, NULL, NULL, huge, sizeof(huge), &ignored),
                 SYS_CODE_INVALID_SIZE));
    CHECK(sys_is(domain_payload_put(&domain, NULL, NULL, NULL, sizeof(written), &ignored),
                 SYS_CODE_INVALID_ARG));

    CHECK(sys_ok(domain_deinit(&domain)));
}

/* Адресат события: тот же механизм, что и у команды (TRANSIENT_PAYLOAD.md §4). */
#define TYPE_DEVICE 1

static void event_carries_target(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 1, 8, CAPACITY, MAX_SIZE)));

    domain_entity_desc_t desc = {0};
    desc.type = TYPE_DEVICE;
    desc.key_size = sizeof(uint64_t);
    desc.payload_size = sizeof(uint32_t);
    desc.capacity = 4;
    desc.backing = DOMAIN_BACKING_RAM;
    CHECK(sys_ok(domain_register_entity(&domain, &desc)));

    inbox_t inbox = {.accept = true};
    domain_subscription_desc_t sub_desc = {0};
    sub_desc.try_push = inbox_push;
    sub_desc.ctx = &inbox;
    domain_subscription_t *sub = NULL;
    CHECK(sys_ok(domain_subscribe(&domain, &sub_desc, &sub)));

    const uint64_t uid = 0x00124B000A1B2C3Dull;
    const domain_fact_target_t target = {.entity = TYPE_DEVICE, .key = &uid};
    const payload_t written = {.bytes = {7}};

    domain_payload_ref_t ref = 0;
    CHECK(sys_ok(domain_payload_put(&domain, &target, NULL, &written, sizeof(written), &ref)));

    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(&domain, &delivered)));
    CHECK(delivered == 1);
    CHECK(inbox.events[0].kind == (uint8_t)DOMAIN_FACT_EVENT);
    CHECK(inbox.events[0].entity == TYPE_DEVICE);
    CHECK(inbox.events[0].key_size == sizeof(uid));
    CHECK(memcmp(inbox.events[0].key, &uid, sizeof(uid)) == 0);

    CHECK(sys_ok(domain_unsubscribe(&domain, sub)));
    CHECK(sys_ok(domain_deinit(&domain)));
}

int main(void)
{
    put_get();
    evicted_payload_is_stale();
    put_publishes_event_with_ref();
    event_carries_target();
    size_mismatch_is_rejected();

    if (failures != 0) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("test_payload: OK\n");
    return 0;
}
