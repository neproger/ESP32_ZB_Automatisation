#include "domain/domain.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

enum {
    TYPE_SENSOR = 1,
    TYPE_SWITCH = 2,
};

typedef struct {
    uint32_t id;
    uint8_t endpoint;
} test_key_t;

typedef struct {
    uint32_t value;
    uint32_t reserved;
} test_record_t;

static domain_entity_desc_t sensor_desc(void)
{
    domain_entity_desc_t desc = {0};
    desc.type = TYPE_SENSOR;
    desc.key_size = sizeof(test_key_t);
    desc.payload_size = sizeof(test_record_t);
    desc.capacity = 8;
    desc.backing = DOMAIN_BACKING_RAM;
    return desc;
}

static void test_init_deinit(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 4) == DOMAIN_OK);
    CHECK(domain_init(&domain, 4) == DOMAIN_INVALID_STATE);
    CHECK(domain_deinit(&domain) == DOMAIN_OK);
    CHECK(domain_deinit(&domain) == DOMAIN_INVALID_STATE);
    CHECK(domain_init(NULL, 4) == DOMAIN_INVALID_ARG);
    CHECK(domain_init(&domain, 0) == DOMAIN_INVALID_ARG);
    (void)domain_deinit(&domain);
}

static void test_register(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 4) == DOMAIN_OK);

    domain_entity_desc_t sensor = sensor_desc();
    CHECK(domain_register_entity(&domain, &sensor) == DOMAIN_OK);

    domain_entity_desc_t duplicate = sensor_desc();
    duplicate.capacity = 16;
    CHECK(domain_register_entity(&domain, &duplicate) == DOMAIN_INVALID_STATE);

    domain_entity_desc_t sw = {0};
    sw.type = TYPE_SWITCH;
    sw.key_size = sizeof(test_key_t);
    sw.payload_size = sizeof(test_record_t);
    sw.capacity = 4;
    sw.backing = DOMAIN_BACKING_RAM;
    CHECK(domain_register_entity(&domain, &sw) == DOMAIN_OK);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

static void test_invalid_desc(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 4) == DOMAIN_OK);

    domain_entity_desc_t no_key = sensor_desc();
    no_key.key_size = 0;
    CHECK(domain_register_entity(&domain, &no_key) == DOMAIN_INVALID_SIZE);

    domain_entity_desc_t no_payload = sensor_desc();
    no_payload.payload_size = 0;
    CHECK(domain_register_entity(&domain, &no_payload) == DOMAIN_INVALID_SIZE);

    domain_entity_desc_t no_capacity = sensor_desc();
    no_capacity.capacity = 0;
    CHECK(domain_register_entity(&domain, &no_capacity) == DOMAIN_INVALID_SIZE);

    domain_entity_desc_t flash_without_key = sensor_desc();
    flash_without_key.backing = DOMAIN_BACKING_FLASH;
    flash_without_key.persist_key = NULL;
    CHECK(domain_register_entity(&domain, &flash_without_key) == DOMAIN_INVALID_ARG);

    domain_entity_desc_t no_backing = sensor_desc();
    no_backing.backing = DOMAIN_BACKING_NONE;
    CHECK(domain_register_entity(&domain, &no_backing) == DOMAIN_INVALID_ARG);

    CHECK(domain_register_entity(&domain, NULL) == DOMAIN_INVALID_ARG);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

static void test_registry_full(void)
{
    domain_t domain = {0};
    CHECK(domain_init(&domain, 2) == DOMAIN_OK);

    domain_entity_desc_t desc = sensor_desc();
    CHECK(domain_register_entity(&domain, &desc) == DOMAIN_OK);

    domain_entity_desc_t second = sensor_desc();
    second.type = TYPE_SWITCH;
    CHECK(domain_register_entity(&domain, &second) == DOMAIN_OK);

    domain_entity_desc_t third = sensor_desc();
    third.type = 3;
    CHECK(domain_register_entity(&domain, &third) == DOMAIN_NO_SPACE);

    CHECK(domain_deinit(&domain) == DOMAIN_OK);
}

int main(void)
{
    test_init_deinit();
    test_register();
    test_invalid_desc();
    test_registry_full();

    if (g_failures != 0) {
        printf("test_registry: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_registry: OK\n");
    return 0;
}
