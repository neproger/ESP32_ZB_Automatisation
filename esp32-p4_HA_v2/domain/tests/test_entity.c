#include "domain/domain.h"

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

static test_record_t record_of(uint32_t value)
{
    test_record_t record = {0};
    record.value = value;
    return record;
}

static domain_entity_desc_t desc_of(domain_entity_t type, size_t capacity)
{
    domain_entity_desc_t desc = {0};
    desc.type = type;
    desc.key_size = sizeof(test_key_t);
    desc.payload_size = sizeof(test_record_t);
    desc.capacity = capacity;
    desc.backing = DOMAIN_BACKING_RAM;
    return desc;
}

static void test_put_changed(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);
    domain_entity_desc_t desc = desc_of(TYPE_A, 4);
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    const test_key_t key = key_of(1);
    test_record_t record = record_of(10);
    bool changed = false;

    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, NULL, &changed) == DOMAIN_OK);
    CHECK(changed);

    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, NULL, &changed) == DOMAIN_OK);
    CHECK(!changed);

    record.value = 11;
    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, NULL, &changed) == DOMAIN_OK);
    CHECK(changed);

    CHECK(domain_entity_put(&domain, TYPE_A, &key, NULL, NULL, &changed) == DOMAIN_INVALID_ARG);
    CHECK(domain_entity_put(&domain, TYPE_A, NULL, &record, NULL, &changed) == DOMAIN_INVALID_ARG);
    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, NULL, NULL) == DOMAIN_INVALID_ARG);
    CHECK(domain_entity_put(&domain, 99, &key, &record, NULL, &changed) == DOMAIN_NOT_FOUND);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

static void test_get_remove(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);
    domain_entity_desc_t desc = desc_of(TYPE_A, 4);
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    const test_key_t key = key_of(1);
    const test_key_t absent = key_of(2);
    test_record_t record = record_of(10);
    test_record_t out = {0};
    bool changed = false;

    CHECK(domain_entity_get(&domain, TYPE_A, &key, &out) == DOMAIN_NOT_FOUND);
    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, NULL, &changed) == DOMAIN_OK);
    CHECK(domain_entity_get(&domain, TYPE_A, &key, &out) == DOMAIN_OK);
    CHECK(out.value == 10);
    CHECK(domain_entity_get(&domain, TYPE_A, &absent, &out) == DOMAIN_NOT_FOUND);
    CHECK(domain_entity_get(&domain, 99, &key, &out) == DOMAIN_NOT_FOUND);
    CHECK(domain_entity_get(&domain, TYPE_A, NULL, &out) == DOMAIN_INVALID_ARG);

    CHECK(domain_entity_remove(&domain, TYPE_A, &key, NULL) == DOMAIN_OK);
    CHECK(domain_entity_get(&domain, TYPE_A, &key, &out) == DOMAIN_NOT_FOUND);
    CHECK(domain_entity_remove(&domain, TYPE_A, &key, NULL) == DOMAIN_NOT_FOUND);

    CHECK(domain_entity_put(&domain, TYPE_A, &key, &record, NULL, &changed) == DOMAIN_OK);
    CHECK(changed);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

static void test_no_space(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);
    domain_entity_desc_t desc = desc_of(TYPE_A, 2);
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    test_record_t record = record_of(1);
    bool changed = false;

    const test_key_t first = key_of(1);
    const test_key_t second = key_of(2);
    const test_key_t third = key_of(3);

    CHECK(domain_entity_put(&domain, TYPE_A, &first, &record, NULL, &changed) == DOMAIN_OK);
    CHECK(domain_entity_put(&domain, TYPE_A, &second, &record, NULL, &changed) == DOMAIN_OK);
    CHECK(domain_entity_put(&domain, TYPE_A, &third, &record, NULL, &changed) == DOMAIN_NO_SPACE);

    CHECK(domain_entity_remove(&domain, TYPE_A, &second, NULL) == DOMAIN_OK);
    CHECK(domain_entity_put(&domain, TYPE_A, &third, &record, NULL, &changed) == DOMAIN_OK);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

typedef struct {
    size_t seen;
    uint32_t sum;
} iter_acc_t;

static bool iter_count(const void *key, const void *record, void *ctx)
{
    (void)key;
    iter_acc_t *acc = (iter_acc_t *)ctx;
    const test_record_t *r = (const test_record_t *)record;
    acc->seen++;
    acc->sum += r->value;
    return true;
}

static bool iter_stop(const void *key, const void *record, void *ctx)
{
    (void)key;
    (void)record;
    (void)ctx;
    return false;
}

static void test_iter(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);
    domain_entity_desc_t desc = desc_of(TYPE_A, 4);
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    iter_acc_t acc = {0};
    CHECK(domain_entity_iter(&domain, TYPE_A, iter_count, &acc) == DOMAIN_OK);
    CHECK(acc.seen == 0);

    test_record_t record = record_of(5);
    bool changed = false;
    const test_key_t first = key_of(1);
    const test_key_t second = key_of(2);
    CHECK(domain_entity_put(&domain, TYPE_A, &first, &record, NULL, &changed) == DOMAIN_OK);
    CHECK(domain_entity_put(&domain, TYPE_A, &second, &record, NULL, &changed) == DOMAIN_OK);

    acc = (iter_acc_t){0};
    CHECK(domain_entity_iter(&domain, TYPE_A, iter_count, &acc) == DOMAIN_OK);
    CHECK(acc.seen == 2);
    CHECK(acc.sum == 10);

    acc = (iter_acc_t){0};
    CHECK(domain_entity_iter(&domain, TYPE_A, iter_stop, &acc) == DOMAIN_OK);
    CHECK(acc.seen == 0);

    CHECK(domain_entity_iter(&domain, TYPE_A, NULL, &acc) == DOMAIN_INVALID_ARG);
    CHECK(domain_entity_iter(&domain, 99, iter_count, &acc) == DOMAIN_NOT_FOUND);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

static void test_two_types_are_independent(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2, 8) == DOMAIN_OK);

    domain_entity_desc_t a = desc_of(TYPE_A, 4);
    domain_entity_desc_t b = desc_of(TYPE_B, 4);
    CHECK(domain_register_entity(&domain, &a) == DOMAIN_OK);
    CHECK(domain_register_entity(&domain, &b) == DOMAIN_OK);

    const test_key_t key = key_of(1);
    test_record_t in_a = record_of(100);
    test_record_t in_b = record_of(200);
    bool changed = false;

    CHECK(domain_entity_put(&domain, TYPE_A, &key, &in_a, NULL, &changed) == DOMAIN_OK);
    CHECK(domain_entity_put(&domain, TYPE_B, &key, &in_b, NULL, &changed) == DOMAIN_OK);

    test_record_t out = {0};
    CHECK(domain_entity_get(&domain, TYPE_A, &key, &out) == DOMAIN_OK);
    CHECK(out.value == 100);
    CHECK(domain_entity_get(&domain, TYPE_B, &key, &out) == DOMAIN_OK);
    CHECK(out.value == 200);

    CHECK(domain_entity_remove(&domain, TYPE_A, &key, NULL) == DOMAIN_OK);
    CHECK(domain_entity_get(&domain, TYPE_A, &key, &out) == DOMAIN_NOT_FOUND);
    CHECK(domain_entity_get(&domain, TYPE_B, &key, &out) == DOMAIN_OK);
    CHECK(out.value == 200);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

int main(void)
{
    test_put_changed();
    test_get_remove();
    test_no_space();
    test_iter();
    test_two_types_are_independent();

    if (g_failures != 0) {
        printf("test_entity: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_entity: OK\n");
    return 0;
}
