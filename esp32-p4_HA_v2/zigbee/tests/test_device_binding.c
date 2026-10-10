#include "zigbee/zigbee_device_binding.h"

#include <stdio.h>
#include <string.h>

/*
 * B3 (docs/STEP_B_PLAN.md): adapter-private binding device_id ↔ EUI64. Биекция, идемпотентность,
 * отказ при конфликте/NONE/нет места, удаление по EUI64. Только форма/логика (host).
 */

#define CAP 4

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define A 0x00124B000A1B2C3Dull
#define B 0x00124B000A1B2C3Eull
#define D1 0x1111111111111111ull
#define D2 0x2222222222222222ull

static void test_add_and_find_both_ways(void)
{
    zb_device_binding_t list[CAP];
    memset(list, 0, sizeof(list));
    size_t n = 0;

    CHECK(zb_device_binding_add(list, CAP, &n, D1, A));
    CHECK(n == 1);

    ha_device_id_t id = HA_DEVICE_ID_NONE;
    CHECK(zb_device_binding_find_device_id(list, n, A, &id) && id == D1);

    ha_device_uid_t uid = 0;
    CHECK(zb_device_binding_find_uid(list, n, D1, &uid) && uid == A);

    CHECK(!zb_device_binding_find_device_id(list, n, B, &id));
    CHECK(!zb_device_binding_find_uid(list, n, D2, &uid));
}

static void test_idempotent_add(void)
{
    zb_device_binding_t list[CAP];
    memset(list, 0, sizeof(list));
    size_t n = 0;

    CHECK(zb_device_binding_add(list, CAP, &n, D1, A));
    CHECK(zb_device_binding_add(list, CAP, &n, D1, A)); /* та же пара — ok */
    CHECK(n == 1);
}

static void test_conflicts_rejected(void)
{
    zb_device_binding_t list[CAP];
    memset(list, 0, sizeof(list));
    size_t n = 0;
    CHECK(zb_device_binding_add(list, CAP, &n, D1, A));

    /* тот же uid под другим device_id — отказ */
    CHECK(!zb_device_binding_add(list, CAP, &n, D2, A));
    /* тот же device_id под другим uid — отказ */
    CHECK(!zb_device_binding_add(list, CAP, &n, D1, B));
    CHECK(n == 1);
}

static void test_none_rejected(void)
{
    zb_device_binding_t list[CAP];
    memset(list, 0, sizeof(list));
    size_t n = 0;
    CHECK(!zb_device_binding_add(list, CAP, &n, HA_DEVICE_ID_NONE, A));
    CHECK(n == 0);
}

static void test_remove_by_uid(void)
{
    zb_device_binding_t list[CAP];
    memset(list, 0, sizeof(list));
    size_t n = 0;
    CHECK(zb_device_binding_add(list, CAP, &n, D1, A));
    CHECK(zb_device_binding_add(list, CAP, &n, D2, B));

    CHECK(zb_device_binding_remove(list, &n, A));
    CHECK(n == 1);
    ha_device_id_t id = HA_DEVICE_ID_NONE;
    CHECK(!zb_device_binding_find_device_id(list, n, A, &id));
    CHECK(zb_device_binding_find_device_id(list, n, B, &id) && id == D2);

    /* повторное удаление — не найдено */
    CHECK(!zb_device_binding_remove(list, &n, A));
}

static void test_capacity(void)
{
    zb_device_binding_t list[2];
    memset(list, 0, sizeof(list));
    size_t n = 0;
    CHECK(zb_device_binding_add(list, 2, &n, D1, A));
    CHECK(zb_device_binding_add(list, 2, &n, D2, B));
    CHECK(!zb_device_binding_add(list, 2, &n, D1 + 5, 0x00124B000A1B2C3Full)); /* нет места */
    CHECK(n == 2);
}

int main(void)
{
    test_add_and_find_both_ways();
    test_idempotent_add();
    test_conflicts_rejected();
    test_none_rejected();
    test_remove_by_uid();
    test_capacity();

    if (g_failures == 0) {
        printf("all device-binding tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
