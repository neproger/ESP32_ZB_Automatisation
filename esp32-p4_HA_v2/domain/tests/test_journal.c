#include "domain_journal.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define CAPACITY 4

static domain_event_t event_of(uint32_t entity, uint32_t value)
{
    domain_event_t event = {0};
    event.ts = 1000 + value;
    event.kind = DOMAIN_FACT_ENTITY_UPSERTED;
    event.op = DOMAIN_OP_ENTITY_PUT;
    event.source = DOMAIN_SOURCE_ZIGBEE;
    event.key_size = 4;
    event.entity = entity;
    memcpy(event.key, &value, sizeof(value));
    event.value.type = DOMAIN_VALUE_U32;
    event.value.v.u32 = value;
    return event;
}

static void test_init_deinit(void)
{
    domain_journal_t journal = {0};
    CHECK(sys_ok(domain_journal_init(&journal, CAPACITY)));
    CHECK(sys_is(domain_journal_init(&journal, CAPACITY), SYS_CODE_INVALID_STATE));

    size_t count = 0;
    CHECK(sys_ok(domain_journal_count(&journal, &count)));
    CHECK(count == 0);

    domain_event_id_t id = 0;
    CHECK(sys_is(domain_journal_oldest(&journal, &id), SYS_CODE_NOT_FOUND));
    CHECK(sys_is(domain_journal_newest(&journal, &id), SYS_CODE_NOT_FOUND));

    CHECK(sys_ok(domain_journal_deinit(&journal)));
}

static void test_append_assigns_identity(void)
{
    domain_journal_t journal = {0};
    CHECK(sys_ok(domain_journal_init(&journal, CAPACITY)));

    domain_event_t event = event_of(1, 42);
    domain_event_id_t id = 0;
    CHECK(sys_ok(domain_journal_append(&journal, &event, &id)));
    CHECK(id == 1);
    CHECK(event.event_id == 1);

    domain_event_t second = event_of(1, 43);
    CHECK(sys_ok(domain_journal_append(&journal, &second, &id)));
    CHECK(id == 2);

    domain_event_t out = {0};
    CHECK(sys_ok(domain_journal_get(&journal, 1, &out)));
    CHECK(out.event_id == 1);
    CHECK(out.ts == 1042);
    CHECK(out.kind == DOMAIN_FACT_ENTITY_UPSERTED);
    CHECK(out.source == DOMAIN_SOURCE_ZIGBEE);
    CHECK(out.entity == 1);
    CHECK(out.key_size == 4);
    CHECK(out.value.type == DOMAIN_VALUE_U32);
    CHECK(out.value.v.u32 == 42);

    uint32_t key_value = 0;
    memcpy(&key_value, out.key, sizeof(key_value));
    CHECK(key_value == 42);

    CHECK(sys_ok(domain_journal_get(&journal, 2, &out)));
    CHECK(out.value.v.u32 == 43);

    CHECK(sys_ok(domain_journal_deinit(&journal)));
}

static void test_bounds_and_gap(void)
{
    domain_journal_t journal = {0};
    CHECK(sys_ok(domain_journal_init(&journal, CAPACITY)));

    domain_event_id_t id = 0;
    domain_event_t out = {0};

    CHECK(sys_is(domain_journal_get(&journal, 1, &out), SYS_CODE_NOT_FOUND));

    for (uint32_t i = 0; i < CAPACITY; ++i) {
        domain_event_t event = event_of(1, i);
        CHECK(sys_ok(domain_journal_append(&journal, &event, &id)));
    }

    domain_event_id_t oldest = 0;
    domain_event_id_t newest = 0;
    CHECK(sys_ok(domain_journal_oldest(&journal, &oldest)));
    CHECK(sys_ok(domain_journal_newest(&journal, &newest)));
    CHECK(oldest == 1);
    CHECK(newest == CAPACITY);

    bool contained = false;
    CHECK(sys_ok(domain_journal_contains(&journal, 1, &contained)));
    CHECK(contained);
    CHECK(sys_ok(domain_journal_contains(&journal, CAPACITY + 1, &contained)));
    CHECK(!contained);

    CHECK(sys_is(domain_journal_get(&journal, CAPACITY + 1, &out), SYS_CODE_NOT_FOUND));

    /* Переполнение: самая старая запись вытеснена — это и есть признак gap. */
    domain_event_t overflow = event_of(1, 99);
    CHECK(sys_ok(domain_journal_append(&journal, &overflow, &id)));
    CHECK(id == CAPACITY + 1);

    size_t count = 0;
    CHECK(sys_ok(domain_journal_count(&journal, &count)));
    CHECK(count == CAPACITY);

    CHECK(sys_ok(domain_journal_oldest(&journal, &oldest)));
    CHECK(oldest == 2);
    CHECK(sys_is(domain_journal_get(&journal, 1, &out), SYS_CODE_STALE));
    CHECK(sys_ok(domain_journal_get(&journal, CAPACITY + 1, &out)));
    CHECK(out.value.v.u32 == 99);

    CHECK(sys_ok(domain_journal_deinit(&journal)));
}

static void test_events_are_not_collapsed(void)
{
    domain_journal_t journal = {0};
    CHECK(sys_ok(domain_journal_init(&journal, CAPACITY)));

    domain_event_id_t first = 0;
    domain_event_id_t last = 0;

    for (uint32_t i = 0; i < CAPACITY; ++i) {
        domain_event_t event = event_of(1, 7); /* один и тот же факт */
        domain_event_id_t id = 0;
        CHECK(sys_ok(domain_journal_append(&journal, &event, &id)));
        if (i == 0) {
            first = id;
        }
        last = id;
    }

    CHECK(last - first == (domain_event_id_t)(CAPACITY - 1));

    size_t count = 0;
    CHECK(sys_ok(domain_journal_count(&journal, &count)));
    CHECK(count == CAPACITY);

    CHECK(sys_ok(domain_journal_deinit(&journal)));
}

static void test_invalid_args(void)
{
    domain_journal_t journal = {0};
    CHECK(sys_is(domain_journal_init(&journal, 0), SYS_CODE_INVALID_ARG));
    CHECK(sys_is(domain_journal_init(NULL, CAPACITY), SYS_CODE_INVALID_ARG));
    CHECK(sys_ok(domain_journal_init(&journal, CAPACITY)));

    domain_event_t event = event_of(1, 1);
    domain_event_id_t id = 0;
    CHECK(sys_is(domain_journal_append(&journal, NULL, &id), SYS_CODE_INVALID_ARG));
    CHECK(sys_is(domain_journal_append(&journal, &event, NULL), SYS_CODE_INVALID_ARG));

    /* Ключ шире лимита — нарушение инварианта, отвергается при регистрации типа,
     * а не здесь: Journal факты не проверяет. */

    CHECK(sys_ok(domain_journal_deinit(&journal)));
}

int main(void)
{
    test_init_deinit();
    test_append_assigns_identity();
    test_bounds_and_gap();
    test_events_are_not_collapsed();
    test_invalid_args();

    if (g_failures != 0) {
        printf("test_journal: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_journal: OK\n");
    return 0;
}
