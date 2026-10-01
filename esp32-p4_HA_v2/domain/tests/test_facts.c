#include "domain/domain.h"

#include <stdio.h>
#include <string.h>

/*
 * White-box: Journal — внутренняя подсистема, публичного пути чтения у него нет
 * (его получит Dispatcher). Пока единственный способ проверить связку
 * «changed=true → факт» — заглянуть в state Domain.
 */
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

static test_key_t key_of(uint32_t id)
{
    test_key_t key = {0};
    key.id = id;
    key.endpoint = 1;
    return key;
}

static void desc_fill(domain_entity_desc_t *out, domain_entity_t type)
{
    memset(out, 0, sizeof(*out));
    out->type = type;
    out->key_size = sizeof(test_key_t);
    out->payload_size = sizeof(test_record_t);
    out->capacity = 8;
    out->backing = DOMAIN_BACKING_RAM;
}

static void meta_fill(domain_fact_meta_t *out, uint8_t source, uint32_t value)
{
    memset(out, 0, sizeof(*out));
    out->source = source;
    out->value.type = DOMAIN_VALUE_U32;
    out->value.v.u32 = value;
}

static size_t fact_count(domain_t *domain)
{
    domain_state_t *state = domain_state(domain);
    size_t count = 0;
    CHECK(domain_journal_count(&state->journal, &count) == DOMAIN_OK);
    return count;
}

static domain_event_t last_fact(domain_t *domain)
{
    domain_state_t *state = domain_state(domain);
    domain_event_id_t newest = 0;
    domain_event_t out = {0};
    CHECK(domain_journal_newest(&state->journal, &newest) == DOMAIN_OK);
    CHECK(domain_journal_get(&state->journal, newest, &out) == DOMAIN_OK);
    return out;
}

static void test_put_writes_fact_only_on_change(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);
    domain_entity_desc_t desc = {0};
    desc_fill(&desc, TYPE_A);
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    const test_key_t key = key_of(1);
    test_record_t record = {0};
    bool changed = false;
    domain_fact_meta_t meta = {0};

    CHECK(fact_count(&domain) == 0);

    record.value = 10;
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 10);
    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed) == DOMAIN_OK);
    CHECK(changed);
    CHECK(fact_count(&domain) == 1);

    /* Повтор того же значения: состояние не изменилось — журнал молчит. */
    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed) == DOMAIN_OK);
    CHECK(!changed);
    CHECK(fact_count(&domain) == 1);

    record.value = 11;
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 11);
    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed) == DOMAIN_OK);
    CHECK(changed);
    CHECK(fact_count(&domain) == 2);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

static void test_fact_carries_identity_and_meta(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);
    domain_entity_desc_t desc = {0};
    desc_fill(&desc, TYPE_A);
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    const test_key_t key = key_of(7);
    test_record_t record = {0};
    record.value = 42;
    bool changed = false;
    domain_fact_meta_t meta = {0};
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 42);
    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed) == DOMAIN_OK);

    const domain_event_t fact = last_fact(&domain);
    CHECK(fact.kind == DOMAIN_FACT_ENTITY_UPSERTED);
    CHECK(fact.op == DOMAIN_OP_UPSERT);
    CHECK(fact.entity == TYPE_A);
    CHECK(fact.source == DOMAIN_SOURCE_ZIGBEE);
    CHECK(fact.value.type == DOMAIN_VALUE_U32);
    CHECK(fact.value.v.u32 == 42);
    CHECK(fact.key_size == sizeof(test_key_t));

    test_key_t carried = {0};
    memcpy(&carried, fact.key, sizeof(carried));
    CHECK(carried.id == 7);
    CHECK(carried.endpoint == 1);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

static void test_remove_writes_fact_with_key(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);
    domain_entity_desc_t desc = {0};
    desc_fill(&desc, TYPE_A);
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    const test_key_t key = key_of(3);
    test_record_t record = {0};
    bool changed = false;
    domain_fact_meta_t meta = {0};
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 1);
    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed) == DOMAIN_OK);
    CHECK(fact_count(&domain) == 1);

    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_UI, 0);
    CHECK(domain_entity_remove(&domain, TYPE_A, &key, &meta) == DOMAIN_OK);
    CHECK(fact_count(&domain) == 2);

    const domain_event_t fact = last_fact(&domain);
    CHECK(fact.kind == DOMAIN_FACT_ENTITY_REMOVED);
    CHECK(fact.op == DOMAIN_OP_REMOVE);
    CHECK(fact.source == DOMAIN_SOURCE_UI);
    CHECK(fact.entity == TYPE_A);

    test_key_t carried = {0};
    memcpy(&carried, fact.key, sizeof(carried));
    CHECK(carried.id == 3);

    /* Повторное удаление: записи нет, значит и факта нет. */
    CHECK(domain_entity_remove(&domain, TYPE_A, &key, &meta) == DOMAIN_NOT_FOUND);
    CHECK(fact_count(&domain) == 2);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

static void test_facts_are_ordered_across_types(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);
    domain_entity_desc_t desc_a = {0};
    domain_entity_desc_t desc_b = {0};
    desc_fill(&desc_a, TYPE_A);
    desc_fill(&desc_b, TYPE_B);
    CHECK(domain_register_entity(&domain, &desc_a) == DOMAIN_OK);
    CHECK(domain_register_entity(&domain, &desc_b) == DOMAIN_OK);

    const test_key_t key = key_of(1);
    test_record_t record = {0};
    bool changed = false;
    domain_fact_meta_t meta = {0};

    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 1);
    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed) == DOMAIN_OK);
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 2);
    CHECK(domain_entity_put(&domain, TYPE_B, &key, &record, &meta, &changed) == DOMAIN_OK);
    CHECK(fact_count(&domain) == 2);

    domain_state_t *state = domain_state(&domain);
    domain_event_id_t oldest = 0;
    domain_event_id_t newest = 0;
    CHECK(domain_journal_oldest(&state->journal, &oldest) == DOMAIN_OK);
    CHECK(domain_journal_newest(&state->journal, &newest) == DOMAIN_OK);
    CHECK(newest == oldest + 1);

    domain_event_t first = {0};
    domain_event_t second = {0};
    CHECK(domain_journal_get(&state->journal, oldest, &first) == DOMAIN_OK);
    CHECK(domain_journal_get(&state->journal, newest, &second) == DOMAIN_OK);
    CHECK(first.entity == TYPE_A);
    CHECK(second.entity == TYPE_B);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

int main(void)
{
    test_put_writes_fact_only_on_change();
    test_fact_carries_identity_and_meta();
    test_remove_writes_fact_with_key();
    test_facts_are_ordered_across_types();

    if (g_failures != 0) {
        printf("test_facts: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_facts: OK\n");
    return 0;
}
