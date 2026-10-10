#include "ha_model/ha_properties.h"

#include <stdio.h>

/*
 * Фаза 0 (docs/PROPERTY_MODEL.md): словарь transport-agnostic. Проверяем стабильность
 * явных id, содержимое дескрипторов и безопасную деградацию для UNKNOWN/невалидного id.
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

/* id и enum'ы — стабильный semantic ABI: явные числа, не автоинкремент. */
_Static_assert(HA_PROPERTY_UNKNOWN == 0, "property id ABI");
_Static_assert(HA_PROPERTY_POWER == 1, "property id ABI");
_Static_assert(HA_PROPERTY_BRIGHTNESS == 2, "property id ABI");
_Static_assert(HA_PROPERTY_TEMPERATURE == 8, "property id ABI");
_Static_assert(HA_PROPERTY_BATTERY_PERCENT == 13, "property id ABI");
_Static_assert(HA_PROPERTY_SYSTEM_TZ_OFFSET == 21, "property id ABI");
_Static_assert(HA_ACTION_NONE == 0, "action id ABI");
_Static_assert(HA_ACTION_SET == 4, "action id ABI");
_Static_assert(HA_EVENT_NONE == 0, "event id ABI");
_Static_assert(HA_EVENT_HOLD == 3, "event id ABI");
_Static_assert(HA_VALUE_NONE == 0, "value kind ABI");
_Static_assert(HA_VALUE_FLOAT == 4, "value kind ABI");
_Static_assert(HA_UNIT_CELSIUS == 1, "unit ABI");
_Static_assert(HA_UNIT_PERCENT == 3, "unit ABI");

static void test_known_descriptors(void)
{
    const ha_property_desc_t *power = ha_property_desc(HA_PROPERTY_POWER);
    CHECK(power->id == HA_PROPERTY_POWER);
    CHECK(power->value_kind == HA_VALUE_BOOL);
    CHECK(power->unit == HA_UNIT_NONE);
    CHECK((power->flags & HA_PROPERTY_FLAG_READ_ONLY) == 0); /* управляемое */

    const ha_property_desc_t *temp = ha_property_desc(HA_PROPERTY_TEMPERATURE);
    CHECK(temp->id == HA_PROPERTY_TEMPERATURE);
    CHECK(temp->value_kind == HA_VALUE_FLOAT);
    CHECK(temp->unit == HA_UNIT_CELSIUS);
    CHECK(temp->flags & HA_PROPERTY_FLAG_READ_ONLY); /* измеряемое */

    const ha_property_desc_t *bright = ha_property_desc(HA_PROPERTY_BRIGHTNESS);
    CHECK(bright->value_kind == HA_VALUE_FLOAT);
    CHECK(bright->unit == HA_UNIT_PERCENT);
    CHECK(bright->range_max == 100.0f);

    const ha_property_desc_t *tz = ha_property_desc(HA_PROPERTY_SYSTEM_TZ_OFFSET);
    CHECK(tz->value_kind == HA_VALUE_I32);
    CHECK(tz->unit == HA_UNIT_MINUTE);
    CHECK(tz->range_min == -840.0f);
}

/* Неизвестный и невалидный id дают валидный указатель на UNKNOWN, не NULL. */
static void test_unknown_and_invalid(void)
{
    const ha_property_desc_t *unk = ha_property_desc(HA_PROPERTY_UNKNOWN);
    CHECK(unk != NULL);
    CHECK(unk->id == HA_PROPERTY_UNKNOWN);
    CHECK(unk->value_kind == HA_VALUE_NONE);

    const ha_property_desc_t *bogus = ha_property_desc((ha_property_id_t)9999);
    CHECK(bogus != NULL);
    CHECK(bogus->id == HA_PROPERTY_UNKNOWN);
    CHECK(bogus->value_kind == HA_VALUE_NONE);

    const ha_property_desc_t *neg = ha_property_desc((ha_property_id_t)-1);
    CHECK(neg != NULL);
    CHECK(neg->id == HA_PROPERTY_UNKNOWN);
}

int main(void)
{
    test_known_descriptors();
    test_unknown_and_invalid();

    if (g_failures == 0) {
        printf("all property-model tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
