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
    CHECK(sys_ok(domain_payload_put(&domain, NULL, &written, sizeof(written), &ref)));
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

    CHECK(sys_ok(domain_payload_put(&domain, NULL, &a, sizeof(a), &first)));
    CHECK(sys_ok(domain_payload_put(&domain, NULL, &b, sizeof(b), &second)));
    /* Ring заполнен: третий вытесняет первый. */
    CHECK(sys_ok(domain_payload_put(&domain, NULL, &c, sizeof(c), &third)));

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
    CHECK(sys_ok(domain_payload_put(&domain, NULL, &written, sizeof(written), &ref)));

    size_t delivered = 0;
    CHECK(sys_ok(domain_dispatch_once(&domain, &delivered)));
    CHECK(delivered == 1);
    CHECK(inbox.count == 1);
    CHECK(inbox.events[0].kind == DOMAIN_FACT_EVENT);
    CHECK(inbox.events[0].op == DOMAIN_OP_PAYLOAD_PUT);
    CHECK(inbox.events[0].payload_ref == ref);

    CHECK(sys_ok(domain_unsubscribe(&domain, sub)));
    CHECK(sys_ok(domain_deinit(&domain)));
}

static void size_mismatch_is_rejected(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 1, 8, CAPACITY, MAX_SIZE)));

    const payload_t written = {.bytes = {5}};
    domain_payload_ref_t ref = 0;
    CHECK(sys_ok(domain_payload_put(&domain, NULL, &written, sizeof(written), &ref)));

    uint8_t small[4] = {0};
    CHECK(sys_is(domain_payload_get(&domain, ref, small, sizeof(small)), SYS_CODE_INVALID_SIZE));

    /* Шире максимума типа payload не принимается. */
    uint8_t huge[64] = {0};
    domain_payload_ref_t ignored = 0;
    CHECK(sys_is(domain_payload_put(&domain, NULL, huge, sizeof(huge), &ignored),
                 SYS_CODE_INVALID_SIZE));
    CHECK(sys_is(domain_payload_put(&domain, NULL, NULL, sizeof(written), &ignored),
                 SYS_CODE_INVALID_ARG));

    CHECK(sys_ok(domain_deinit(&domain)));
}

int main(void)
{
    put_get();
    evicted_payload_is_stale();
    put_publishes_event_with_ref();
    size_mismatch_is_rejected();

    if (failures != 0) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("test_payload: OK\n");
    return 0;
}
