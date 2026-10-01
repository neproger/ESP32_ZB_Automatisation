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
    CHECK(domain_journal_init(&journal, CAPACITY) == DOMAIN_OK);
    CHECK(domain_journal_init(&journal, CAPACITY) == DOMAIN_INVALID_STATE);

    size_t count = 0;
    CHECK(domain_journal_count(&journal, &count) == DOMAIN_OK);
    CHECK(count == 0);

    domain_event_id_t id = 0;
    CHECK(domain_journal_oldest(&journal, &id) == DOMAIN_NOT_FOUND);
    CHECK(domain_journal_newest(&journal, &id) == DOMAIN_NOT_FOUND);

    CHECK(domain_journal_deinit(&journal) == DOMAIN_OK);
}

static void test_append_assigns_identity(void)
{
    domain_journal_t journal = {0};
    CHECK(domain_journal_init(&journal, CAPACITY) == DOMAIN_OK);

    domain_event_t event = event_of(1, 42);
    domain_event_id_t id = 0;
    CHECK(domain_journal_append(&journal, &event, &id) == DOMAIN_OK);
    CHECK(id == 1);
    CHECK(event.event_id == 1);

    domain_event_t second = event_of(1, 43);
    CHECK(domain_journal_append(&journal, &second, &id) == DOMAIN_OK);
    CHECK(id == 2);

    domain_event_t out = {0};
    CHECK(domain_journal_get(&journal, 1, &out) == DOMAIN_OK);
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

    CHECK(domain_journal_get(&journal, 2, &out) == DOMAIN_OK);
    CHECK(out.value.v.u32 == 43);

    CHECK(domain_journal_deinit(&journal) == DOMAIN_OK);
}

static void test_bounds_and_gap(void)
{
    domain_journal_t journal = {0};
    CHECK(domain_journal_init(&journal, CAPACITY) == DOMAIN_OK);

    domain_event_id_t id = 0;
    domain_event_t out = {0};

    CHECK(domain_journal_get(&journal, 1, &out) == DOMAIN_NOT_FOUND);

    for (uint32_t i = 0; i < CAPACITY; ++i) {
        domain_event_t event = event_of(1, i);
        CHECK(domain_journal_append(&journal, &event, &id) == DOMAIN_OK);
    }

    domain_event_id_t oldest = 0;
    domain_event_id_t newest = 0;
    CHECK(domain_journal_oldest(&journal, &oldest) == DOMAIN_OK);
    CHECK(domain_journal_newest(&journal, &newest) == DOMAIN_OK);
    CHECK(oldest == 1);
    CHECK(newest == CAPACITY);

    bool contained = false;
    CHECK(domain_journal_contains(&journal, 1, &contained) == DOMAIN_OK);
    CHECK(contained);
    CHECK(domain_journal_contains(&journal, CAPACITY + 1, &contained) == DOMAIN_OK);
    CHECK(!contained);

    CHECK(domain_journal_get(&journal, CAPACITY + 1, &out) == DOMAIN_NOT_FOUND);

    /* Переполнение: самая старая запись вытеснена — это и есть признак gap. */
    domain_event_t overflow = event_of(1, 99);
    CHECK(domain_journal_append(&journal, &overflow, &id) == DOMAIN_OK);
    CHECK(id == CAPACITY + 1);

    size_t count = 0;
    CHECK(domain_journal_count(&journal, &count) == DOMAIN_OK);
    CHECK(count == CAPACITY);

    CHECK(domain_journal_oldest(&journal, &oldest) == DOMAIN_OK);
    CHECK(oldest == 2);
    CHECK(domain_journal_get(&journal, 1, &out) == DOMAIN_STALE);
    CHECK(domain_journal_get(&journal, CAPACITY + 1, &out) == DOMAIN_OK);
    CHECK(out.value.v.u32 == 99);

    CHECK(domain_journal_deinit(&journal) == DOMAIN_OK);
}

static void test_events_are_not_collapsed(void)
{
    domain_journal_t journal = {0};
    CHECK(domain_journal_init(&journal, CAPACITY) == DOMAIN_OK);

    domain_event_id_t first = 0;
    domain_event_id_t last = 0;

    for (uint32_t i = 0; i < CAPACITY; ++i) {
        domain_event_t event = event_of(1, 7); /* один и тот же факт */
        domain_event_id_t id = 0;
        CHECK(domain_journal_append(&journal, &event, &id) == DOMAIN_OK);
        if (i == 0) {
            first = id;
        }
        last = id;
    }

    CHECK(last - first == (domain_event_id_t)(CAPACITY - 1));

    size_t count = 0;
    CHECK(domain_journal_count(&journal, &count) == DOMAIN_OK);
    CHECK(count == CAPACITY);

    CHECK(domain_journal_deinit(&journal) == DOMAIN_OK);
}

static void test_invalid_args(void)
{
    domain_journal_t journal = {0};
    CHECK(domain_journal_init(&journal, 0) == DOMAIN_INVALID_ARG);
    CHECK(domain_journal_init(NULL, CAPACITY) == DOMAIN_INVALID_ARG);
    CHECK(domain_journal_init(&journal, CAPACITY) == DOMAIN_OK);

    domain_event_t event = event_of(1, 1);
    domain_event_id_t id = 0;
    CHECK(domain_journal_append(&journal, NULL, &id) == DOMAIN_INVALID_ARG);
    CHECK(domain_journal_append(&journal, &event, NULL) == DOMAIN_INVALID_ARG);

    /* Ключ шире лимита — нарушение инварианта, отвергается при регистрации типа,
     * а не здесь: Journal факты не проверяет. */

    CHECK(domain_journal_deinit(&journal) == DOMAIN_OK);
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
