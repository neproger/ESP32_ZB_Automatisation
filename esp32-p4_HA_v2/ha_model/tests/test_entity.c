#include "ha_model/ha_entity.h"

#include <stddef.h>
#include <stdio.h>

/*
 * A0.1 (docs/STEP_A_PLAN.md): transport-agnostic Entity contract.
 * Проверяем только форму/типы; способ вывода entity_id — A0.3.
 */

_Static_assert(sizeof(ha_entity_id_t) == 8, "entity id ABI");
_Static_assert(sizeof(ha_entity_key_t) == 8, "entity key ABI");
_Static_assert(offsetof(ha_entity_key_t, id) == 0, "entity key layout");
_Static_assert(HA_ENTITY_ID_NONE == 0, "entity none sentinel");

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

static void test_key(void)
{
    ha_entity_key_t key = {.id = 0x00124B000A1B2C3Dull};
    CHECK(key.id == 0x00124B000A1B2C3Dull);

    ha_entity_key_t none = {.id = HA_ENTITY_ID_NONE};
    CHECK(none.id == 0u);

    /* копирование побайтовым memcpy (каноническое равенство payload) */
    ha_entity_key_t copy = {0};
    copy = key;
    CHECK(copy.id == key.id);
}

int main(void)
{
    test_key();

    if (g_failures == 0) {
        printf("all entity tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
