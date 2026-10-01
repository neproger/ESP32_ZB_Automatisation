#include "domain/domain.h"

#include <stdio.h>
#include <string.h>

#include "nor_sim.h"
#include "nor_sim_device.h"
#include "mstore_platform.h"

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define SIM_SIZE (64u * 1024u)
#define SIM_ERASE_SIZE 4096u

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
    uint32_t value;
    uint32_t extra[16];
} test_record_wide_t;

typedef struct {
    size_t seen;
} count_ctx_t;

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

static bool count_cb(const void *key, const void *record, void *ctx)
{
    (void)key;
    (void)record;
    ((count_ctx_t *)ctx)->seen++;
    return true;
}

static size_t record_count(domain_t *domain, domain_entity_t type)
{
    count_ctx_t ctx = {0};
    CHECK(sys_ok(domain_entity_iter(domain, type, count_cb, &ctx)));
    return ctx.seen;
}

static domain_entity_desc_t desc_of(domain_entity_t type, const char *persist_key,
                                    size_t payload_size, size_t capacity)
{
    domain_entity_desc_t desc = {0};
    desc.type = type;
    desc.key_size = sizeof(test_key_t);
    desc.payload_size = payload_size;
    desc.capacity = capacity;
    desc.backing = DOMAIN_BACKING_FLASH;
    desc.persist_key = persist_key;
    return desc;
}

static void flash_on(mstore_nor_sim_t **out_sim, nor_sim_device_t *device)
{
    *out_sim = mstore_nor_sim_create(SIM_SIZE, SIM_ERASE_SIZE);
    CHECK(*out_sim != NULL);
    nor_sim_device_init(device, *out_sim);
    mstore_platform_flash_set_device(&device->base);
}

static void flash_off(mstore_nor_sim_t *sim)
{
    mstore_platform_flash_set_device(NULL);
    mstore_nor_sim_destroy(sim);
}

static void test_reopen_preserves_state(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    const test_key_t key1 = key_of(1);
    const test_key_t key2 = key_of(2);
    bool changed = false;

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 8, 8, 64)));
    domain_entity_desc_t desc = desc_of(TYPE_A, "test_entity", sizeof(test_record_t), 8);
    CHECK(sys_ok(domain_register_entity(&domain, &desc)));

    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key1, &(test_record_t){.value = 10}, NULL,
                            &changed)));
    CHECK(changed);
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key2, &(test_record_t){.value = 20}, NULL,
                            &changed)));
    CHECK(changed);

    test_record_t updated = record_of(11);
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key1, &updated, NULL, &changed)));
    CHECK(changed);
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key1, &updated, NULL, &changed)));
    CHECK(!changed);

    CHECK(sys_ok(domain_entity_remove(&domain, TYPE_A, &key2, NULL)));
    CHECK(sys_ok(domain_deinit(&domain)));

    domain_t reopened = {0};
    CHECK(sys_ok(domain_init(&reopened, 2, 8, 8, 64)));
    CHECK(sys_ok(domain_register_entity(&reopened, &desc)));

    test_record_t out = {0};
    CHECK(sys_ok(domain_entity_get(&reopened, TYPE_A, &key1, &out)));
    CHECK(out.value == 11);
    CHECK(sys_is(domain_entity_get(&reopened, TYPE_A, &key2, &out), SYS_CODE_NOT_FOUND));
    CHECK(record_count(&reopened, TYPE_A) == 1);

    CHECK(sys_ok(domain_deinit(&reopened)));
    flash_off(sim);
}

static void test_geometry_mismatch_is_rejected(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 8, 8, 64)));
    domain_entity_desc_t narrow = desc_of(TYPE_A, "test_entity", sizeof(test_record_t), 8);
    CHECK(sys_ok(domain_register_entity(&domain, &narrow)));
    CHECK(sys_ok(domain_deinit(&domain)));

    domain_t reopened = {0};
    CHECK(sys_ok(domain_init(&reopened, 2, 8, 8, 64)));
    domain_entity_desc_t wide = desc_of(TYPE_A, "test_entity", sizeof(test_record_wide_t), 64);
    /* Region Manager не меняет размер региона на месте: смена геометрии — диагноз,
     * а не миграция (MSTORE_FLASH_REGIONS.md §6). */
    CHECK(sys_is(domain_register_entity(&reopened, &wide), SYS_CODE_INVALID_SIZE));
    CHECK(sys_ok(domain_deinit(&reopened)));

    flash_off(sim);
}

static void test_two_flash_types_are_independent(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    const test_key_t key = key_of(1);
    bool changed = false;

    domain_t domain = {0};
    CHECK(sys_ok(domain_init(&domain, 2, 8, 8, 64)));
    domain_entity_desc_t a = desc_of(TYPE_A, "type_a", sizeof(test_record_t), 8);
    domain_entity_desc_t b = desc_of(TYPE_B, "type_b", sizeof(test_record_t), 8);
    CHECK(sys_ok(domain_register_entity(&domain, &a)));
    CHECK(sys_ok(domain_register_entity(&domain, &b)));

    CHECK(sys_ok(domain_entity_put(&domain, TYPE_A, &key, &(test_record_t){.value = 100}, NULL,
                            &changed)));
    CHECK(sys_ok(domain_entity_put(&domain, TYPE_B, &key, &(test_record_t){.value = 200}, NULL,
                            &changed)));
    CHECK(sys_ok(domain_deinit(&domain)));

    domain_t reopened = {0};
    CHECK(sys_ok(domain_init(&reopened, 2, 8, 8, 64)));
    CHECK(sys_ok(domain_register_entity(&reopened, &a)));
    CHECK(sys_ok(domain_register_entity(&reopened, &b)));

    test_record_t out = {0};
    CHECK(sys_ok(domain_entity_get(&reopened, TYPE_A, &key, &out)));
    CHECK(out.value == 100);
    CHECK(sys_ok(domain_entity_get(&reopened, TYPE_B, &key, &out)));
    CHECK(out.value == 200);
    CHECK(record_count(&reopened, TYPE_A) == 1);
    CHECK(record_count(&reopened, TYPE_B) == 1);

    CHECK(sys_ok(domain_entity_remove(&reopened, TYPE_A, &key, NULL)));
    CHECK(sys_is(domain_entity_get(&reopened, TYPE_A, &key, &out), SYS_CODE_NOT_FOUND));
    CHECK(sys_ok(domain_entity_get(&reopened, TYPE_B, &key, &out)));
    CHECK(out.value == 200);

    CHECK(sys_ok(domain_deinit(&reopened)));
    flash_off(sim);
}

int main(void)
{
    test_reopen_preserves_state();
    test_geometry_mismatch_is_rejected();
    test_two_flash_types_are_independent();

    if (g_failures != 0) {
        printf("test_flash_reopen: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_flash_reopen: OK\n");
    return 0;
}
