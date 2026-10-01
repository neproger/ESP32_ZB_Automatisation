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
    CHECK(sys_ok(domain_journal_count(&state->journal, &count)));
    return count;
}

static domain_event_t last_fact(domain_t *domain)
{
    domain_state_t *state = domain_state(domain);
    domain_event_id_t newest = 0;
    domain_event_t out = {0};
    CHECK(sys_ok(domain_journal_newest(&state->journal, &newest)));
    CHECK(sys_ok(domain_journal_get(&state->journal, newest, &out)));
    return out;
}

static void test_put_writes_fact_only_on_change(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 8, 8, 64)));
    domain_entity_desc_t desc = {0};
    desc_fill(&desc, TYPE_A);
    CHECK(sys_ok(domain_register_entity(&domain, &desc)));

    const test_key_t key = key_of(1);
    test_record_t record = {0};
    bool changed = false;
    domain_fact_meta_t meta = {0};

    CHECK(fact_count(&domain) == 0);

    record.value = 10;
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 10);
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed)));
    CHECK(changed);
    CHECK(fact_count(&domain) == 1);

    /* Повтор того же значения: состояние не изменилось — журнал молчит. */
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed)));
    CHECK(!changed);
    CHECK(fact_count(&domain) == 1);

    record.value = 11;
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 11);
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed)));
    CHECK(changed);
    CHECK(fact_count(&domain) == 2);

    CHECK(sys_ok(domain_deinit(&domain)));
}

static void test_fact_carries_identity_and_meta(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 8, 8, 64)));
    domain_entity_desc_t desc = {0};
    desc_fill(&desc, TYPE_A);
    CHECK(sys_ok(domain_register_entity(&domain, &desc)));

    const test_key_t key = key_of(7);
    test_record_t record = {0};
    record.value = 42;
    bool changed = false;
    domain_fact_meta_t meta = {0};
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 42);
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed)));

    const domain_event_t fact = last_fact(&domain);
    CHECK(fact.kind == DOMAIN_FACT_ENTITY_UPSERTED);
    CHECK(fact.op == DOMAIN_OP_ENTITY_PUT);
    CHECK(fact.entity == TYPE_A);
    CHECK(fact.source == DOMAIN_SOURCE_ZIGBEE);
    CHECK(fact.value.type == DOMAIN_VALUE_U32);
    CHECK(fact.value.v.u32 == 42);
    CHECK(fact.key_size == sizeof(test_key_t));

    test_key_t carried = {0};
    memcpy(&carried, fact.key, sizeof(carried));
    CHECK(carried.id == 7);
    CHECK(carried.endpoint == 1);

    CHECK(sys_ok(domain_deinit(&domain)));
}

static void test_remove_writes_fact_with_key(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 8, 8, 64)));
    domain_entity_desc_t desc = {0};
    desc_fill(&desc, TYPE_A);
    CHECK(sys_ok(domain_register_entity(&domain, &desc)));

    const test_key_t key = key_of(3);
    test_record_t record = {0};
    bool changed = false;
    domain_fact_meta_t meta = {0};
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 1);
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed)));
    CHECK(fact_count(&domain) == 1);

    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_UI, 0);
    CHECK(sys_ok(domain_entity_remove(&domain, TYPE_A, &key, &meta)));
    CHECK(fact_count(&domain) == 2);

    const domain_event_t fact = last_fact(&domain);
    CHECK(fact.kind == DOMAIN_FACT_ENTITY_REMOVED);
    CHECK(fact.op == DOMAIN_OP_ENTITY_REMOVE);
    CHECK(fact.source == DOMAIN_SOURCE_UI);
    CHECK(fact.entity == TYPE_A);

    test_key_t carried = {0};
    memcpy(&carried, fact.key, sizeof(carried));
    CHECK(carried.id == 3);

    /* Повторное удаление: записи нет, значит и факта нет. */
    CHECK(sys_is(domain_entity_remove(&domain, TYPE_A, &key, &meta), SYS_CODE_NOT_FOUND));
    CHECK(fact_count(&domain) == 2);

    CHECK(sys_ok(domain_deinit(&domain)));
}

static void test_facts_are_ordered_across_types(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 8, 8, 64)));
    domain_entity_desc_t desc_a = {0};
    domain_entity_desc_t desc_b = {0};
    desc_fill(&desc_a, TYPE_A);
    desc_fill(&desc_b, TYPE_B);
    CHECK(sys_ok(domain_register_entity(&domain, &desc_a)));
    CHECK(sys_ok(domain_register_entity(&domain, &desc_b)));

    const test_key_t key = key_of(1);
    test_record_t record = {0};
    bool changed = false;
    domain_fact_meta_t meta = {0};

    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 1);
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed)));
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 2);
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_B, &key, &record, &meta, &changed)));
    CHECK(fact_count(&domain) == 2);

    domain_state_t *state = domain_state(&domain);
    domain_event_id_t oldest = 0;
    domain_event_id_t newest = 0;
    CHECK(sys_ok(domain_journal_oldest(&state->journal, &oldest)));
    CHECK(sys_ok(domain_journal_newest(&state->journal, &newest)));
    CHECK(newest == oldest + 1);

    domain_event_t first = {0};
    domain_event_t second = {0};
    CHECK(sys_ok(domain_journal_get(&state->journal, oldest, &first)));
    CHECK(sys_ok(domain_journal_get(&state->journal, newest, &second)));
    CHECK(first.entity == TYPE_A);
    CHECK(second.entity == TYPE_B);

    CHECK(sys_ok(domain_deinit(&domain)));
}

static void test_runtime_error_is_recorded(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 8, 8, 64)));

    domain_entity_desc_t desc = {0};
    desc_fill(&desc, TYPE_A);
    desc.capacity = 1; /* вторая запись не влезет */
    CHECK(sys_ok(domain_register_entity(&domain, &desc)));

    const test_key_t first = key_of(1);
    const test_key_t second = key_of(2);
    test_record_t record = {0};
    bool changed = false;
    domain_fact_meta_t meta = {0};
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_ZIGBEE, 1);

    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &first, &record, &meta, &changed)));
    CHECK(fact_count(&domain) == 1);

    CHECK(sys_is(domain_entity_put(&domain, TYPE_A, &second, &record, &meta, &changed), SYS_CODE_NO_SPACE));
    CHECK(fact_count(&domain) == 2);

    const domain_event_t fact = last_fact(&domain);
    CHECK(fact.kind == DOMAIN_FACT_ERROR);
    CHECK(fact.op == DOMAIN_OP_ENTITY_PUT);
    CHECK(sys_is(fact.error, SYS_CODE_NO_SPACE));
    CHECK(fact.entity == TYPE_A);

    test_key_t carried = {0};
    memcpy(&carried, fact.key, sizeof(carried));
    CHECK(carried.id == 2);

    CHECK(sys_ok(domain_deinit(&domain)));
}

static void test_programming_errors_are_not_recorded(void)
{
    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 8, 8, 64)));
    domain_entity_desc_t desc = {0};
    desc_fill(&desc, TYPE_A);
    CHECK(sys_ok(domain_register_entity(&domain, &desc)));

    const test_key_t key = key_of(1);
    const test_key_t absent = key_of(9);
    test_record_t record = {0};
    test_record_t out = {0};
    bool changed = false;
    domain_fact_meta_t meta = {0};
    meta_fill(&meta, (uint8_t)DOMAIN_SOURCE_UI, 1);

    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, &changed)));
    const size_t after_success = fact_count(&domain);

    /* NULL и неверные аргументы — ошибки программирования, в Journal не идут. */
    CHECK(sys_is(domain_entity_put(&domain, TYPE_A, &key, NULL, &meta, &changed), SYS_CODE_INVALID_ARG));
    CHECK(sys_is(domain_entity_put(&domain, TYPE_A, &key, &record, &meta, NULL), SYS_CODE_INVALID_ARG));

    /* Удаление отсутствующего ключа — нормальный исход, а не сбой системы. */
    CHECK(sys_is(domain_entity_remove(&domain, TYPE_A, &absent, &meta), SYS_CODE_NOT_FOUND));

    CHECK(sys_is(domain_entity_get(&domain, TYPE_A, &absent, &out), SYS_CODE_NOT_FOUND));
    CHECK(fact_count(&domain) == after_success);

    CHECK(sys_ok(domain_deinit(&domain)));
}

int main(void)
{
    test_put_writes_fact_only_on_change();
    test_fact_carries_identity_and_meta();
    test_remove_writes_fact_with_key();
    test_facts_are_ordered_across_types();
    test_runtime_error_is_recorded();
    test_programming_errors_are_not_recorded();

    if (g_failures != 0) {
        printf("test_facts: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_facts: OK\n");
    return 0;
}
