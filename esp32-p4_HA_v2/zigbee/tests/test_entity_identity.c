#include "zigbee/zigbee_entity_binding.h"

#include <stdio.h>
#include <string.h>

#include "domain/domain.h"
#include "mstore_platform.h"
#include "nor_sim.h"
#include "nor_sim_device.h"
#include "storage/mstore_storage.h"

#define SIM_SIZE (64u * 1024u)
#define SIM_ERASE_SIZE 4096u

static void flash_on(mstore_nor_sim_t **out_sim, nor_sim_device_t *device)
{
    *out_sim = mstore_nor_sim_create(SIM_SIZE, SIM_ERASE_SIZE);
    nor_sim_device_init(device, *out_sim);
    mstore_platform_flash_set_device(&device->base);
}

/*
 * A0.5: доказательство стабильности logical Entity identity на уровне интеграции
 * (resolve binding + ensure Entity в настоящем in-memory Domain).
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define REG_CAP 8
#define A 0x00124B000A1B2C3Dull
#define Y 0x00124B000A1B2C3Eull

typedef struct {
    domain_t d;
    zb_entity_binding_t reg[REG_CAP];
    size_t n;
} ctx_t;

static void ctx_init(ctx_t *c, size_t entity_cap)
{
    memset(c, 0, sizeof(*c)); /* domain_t._state обязан быть NULL до init */
    domain_init(&c->d, 4, 16, 8, 64);
    const domain_entity_desc_t desc = {
        .type = (domain_entity_t)HA_ENTITY_ENTITY,
        .key_size = sizeof(ha_entity_key_t),
        .payload_size = sizeof(ha_entity_record_t),
        .capacity = entity_cap,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    domain_register_entity(&c->d, &desc);
    c->n = 0;
}

static bool domain_has(ctx_t *c, ha_entity_id_t id)
{
    const ha_entity_key_t k = {.id = id};
    ha_entity_record_t r;
    return sys_ok(domain_entity_get(&c->d, (domain_entity_t)HA_ENTITY_ENTITY, &k, &r));
}

static ha_entity_id_t ensure(ctx_t *c, uint64_t uid, uint8_t ep, sys_error_t *err)
{
    ha_entity_id_t id = HA_ENTITY_ID_NONE;
    *err = zb_entity_ensure_entity(&c->d, c->reg, REG_CAP, &c->n, uid, ep, &id);
    return id;
}

/* 1-2: first discovery + repeated interview (идемпотентно). */
static void test_first_and_repeat(void)
{
    ctx_t c;
    ctx_init(&c, 4);
    sys_error_t e;

    const ha_entity_id_t id = ensure(&c, A, 1, &e);
    CHECK(sys_ok(e) && id != HA_ENTITY_ID_NONE && c.n == 1 && domain_has(&c, id));

    const ha_entity_id_t id2 = ensure(&c, A, 1, &e);
    CHECK(sys_ok(e) && id2 == id && c.n == 1); /* не растёт, вторая Entity не создаётся */
}

/* 3: порядок discovery не влияет. */
static void test_order(void)
{
    ctx_t a, b;
    ctx_init(&a, 4);
    ctx_init(&b, 4);
    sys_error_t e;

    const ha_entity_id_t ax1 = ensure(&a, A, 1, &e);
    const ha_entity_id_t ay2 = ensure(&a, Y, 2, &e);
    const ha_entity_id_t by2 = ensure(&b, Y, 2, &e); /* обратный порядок */
    const ha_entity_id_t bx1 = ensure(&b, A, 1, &e);

    CHECK(ax1 == bx1 && ay2 == by2);
}

/* 4-5: reboot / re-pair (свежий registry+Domain, без persistence) → та же identity. */
static void test_reboot_repair(void)
{
    ctx_t c0;
    ctx_init(&c0, 4);
    sys_error_t e;
    const ha_entity_id_t before = ensure(&c0, A, 1, &e);

    ctx_t c1; /* «reboot»: пустые registry и Domain */
    ctx_init(&c1, 4);
    const ha_entity_id_t after = ensure(&c1, A, 1, &e);
    CHECK(after == before);
}

/* 6: разные endpoint → разные id. */
static void test_endpoint_change(void)
{
    ctx_t c;
    ctx_init(&c, 4);
    sys_error_t e;
    const ha_entity_id_t e1 = ensure(&c, A, 1, &e);
    const ha_entity_id_t e2 = ensure(&c, A, 2, &e);
    CHECK(e1 != e2);
}

/* 8: failure/atomicity — resolve создал привязку, ensure Entity упал → откат. */
static void test_atomicity(void)
{
    ctx_t c;
    ctx_init(&c, 1); /* Domain Entity capacity = 1 */
    sys_error_t e;

    const ha_entity_id_t x1 = ensure(&c, A, 1, &e);
    CHECK(sys_ok(e) && domain_has(&c, x1) && c.n == 1);

    /* вычислим id для (Y,2) отдельно (throwaway resolve), чтобы проверить отсутствие Entity */
    zb_entity_binding_t scratch[REG_CAP] = {0};
    size_t sn = 0;
    ha_entity_id_t y2 = HA_ENTITY_ID_NONE;
    CHECK(zb_entity_resolve(scratch, REG_CAP, &sn, Y, 2, &y2, NULL));

    const ha_entity_id_t got = ensure(&c, Y, 2, &e); /* Domain полон → put fail */
    CHECK(sys_failed(e));
    /* откат: привязка (Y,2) не осталась, ложной Entity нет, существующая не тронута */
    CHECK(c.n == 1 && !domain_has(&c, y2) && domain_has(&c, x1));
    (void)got;
}

int main(void)
{
    mstore_nor_sim_t *sim = NULL;
    nor_sim_device_t device;
    flash_on(&sim, &device);

    test_first_and_repeat();
    test_order();
    test_reboot_repair();
    test_endpoint_change();
    test_atomicity();

    mstore_platform_flash_set_device(NULL);
    mstore_nor_sim_destroy(sim);

    if (g_failures == 0) {
        printf("all entity-identity tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
