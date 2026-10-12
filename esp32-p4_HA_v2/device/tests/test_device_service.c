#include "device/device_service.h"

#include <stdio.h>
#include <string.h>

#include "ha_model/ha_automation.h"
#include "ha_model/ha_capabilities.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_rule.h"
#include "ha_model/ha_state.h"

/* Adapter-owned source id: в тесте произвольный (generic core его не именует). */
#define TEST_SOURCE ((ha_source_id_t)1)

/*
 * B3' (docs/STEP_B_PLAN.md): generic Device service над Domain. Нормализованный desc →
 * canonical identity; ownership Entity; удаление Device. Транспорта не знает (source/native_id
 * opaque). Source of truth — Domain (in-memory в тесте).
 */

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                          \
        }                                                          \
    } while (0)

#define ROOT_NATIVE "00124b000a1b2c3d"
#define CHILD_NATIVE "00124b000a1b2c3d/channel/1"

static void put_entity(domain_t *d, ha_entity_id_t id, const char *name)
{
    const ha_entity_key_t key = {.id = id};
    ha_entity_record_t rec = {0};
    size_t n = strlen(name);
    if (n >= sizeof(rec.name)) {
        n = sizeof(rec.name) - 1;
    }
    memcpy(rec.name, name, n);
    domain_fact_meta_t meta = {0};
    bool changed = false;
    (void)domain_entity_put(d, (domain_entity_t)HA_ENTITY_ENTITY, &key, &rec, &meta, &changed);
}

static bool get_device(domain_t *d, ha_device_id_t id, ha_logical_device_record_t *out)
{
    const ha_logical_device_key_t key = {.id = id};
    return sys_ok(
        domain_entity_get(d, (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE, &key, out));
}

static bool get_entity_device(domain_t *d, ha_entity_id_t id, ha_device_id_t *out)
{
    const ha_entity_key_t key = {.id = id};
    ha_entity_record_t rec = {0};
    if (sys_failed(domain_entity_get(d, (domain_entity_t)HA_ENTITY_ENTITY, &key, &rec))) {
        return false;
    }
    *out = rec.device_id;
    return true;
}

static void ctx_init(domain_t *d)
{
    domain_init(d, 8, 16, 8, 64);
    const domain_entity_desc_t entity_desc = {
        .type = (domain_entity_t)HA_ENTITY_ENTITY,
        .key_size = sizeof(ha_entity_key_t),
        .payload_size = sizeof(ha_entity_record_t),
        .capacity = 8,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    const domain_entity_desc_t device_desc = {
        .type = (domain_entity_t)HA_ENTITY_LOGICAL_DEVICE,
        .key_size = sizeof(ha_logical_device_key_t),
        .payload_size = sizeof(ha_logical_device_record_t),
        .capacity = 8,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    domain_register_entity(d, &entity_desc);
    domain_register_entity(d, &device_desc);

    const domain_entity_desc_t ps_desc = {
        .type = (domain_entity_t)HA_ENTITY_PROPERTY_STATE,
        .key_size = sizeof(ha_state_key_t),
        .payload_size = sizeof(ha_state_record_t),
        .capacity = 16,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    const domain_entity_desc_t caps_desc = {
        .type = (domain_entity_t)HA_ENTITY_CAPABILITIES,
        .key_size = sizeof(ha_entity_key_t),
        .payload_size = sizeof(ha_entity_capabilities_record_t),
        .capacity = 16,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    const domain_entity_desc_t gi_desc = {
        .type = (domain_entity_t)HA_ENTITY_GROUP_ITEM,
        .key_size = sizeof(ha_group_item_key_t),
        .payload_size = sizeof(ha_group_item_record_t),
        .capacity = 16,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    const domain_entity_desc_t auto_desc = {
        .type = (domain_entity_t)HA_ENTITY_AUTOMATION,
        .key_size = sizeof(ha_automation_key_t),
        .payload_size = sizeof(ha_rule_t),
        .capacity = 8,
        .backing = DOMAIN_BACKING_RAM,
        .persist_key = NULL,
    };
    domain_register_entity(d, &ps_desc);
    domain_register_entity(d, &caps_desc);
    domain_register_entity(d, &gi_desc);
    domain_register_entity(d, &auto_desc);
}

static void test_register_and_idempotent(void)
{
    domain_t d;
    memset(&d, 0, sizeof(d));
    ctx_init(&d);

    const ha_device_desc_t desc = {
        .source = TEST_SOURCE,
        .native_id = ROOT_NATIVE,
        .parent_native_id = "",
        .name = "Реле",
    };
    ha_device_id_t id = HA_DEVICE_ID_NONE;
    CHECK(sys_ok(device_register(&d, DOMAIN_SOURCE_UI, &desc, &id)));
    CHECK(id == ha_device_id_derive(TEST_SOURCE, ROOT_NATIVE));

    ha_logical_device_record_t rec = {0};
    CHECK(get_device(&d, id, &rec));
    CHECK(rec.source == TEST_SOURCE);
    CHECK(rec.parent_device_id == HA_DEVICE_ID_NONE);
    CHECK(strcmp(rec.native_id, ROOT_NATIVE) == 0);
    CHECK(strcmp(rec.name, "Реле") == 0);

    /* Идемпотентно: другое имя не перетирает существующее. */
    ha_device_desc_t rename = desc;
    memcpy(rename.name, "Другое", 7); /* включая NUL */
    ha_device_id_t id2 = HA_DEVICE_ID_NONE;
    CHECK(sys_ok(device_register(&d, DOMAIN_SOURCE_UI, &rename, &id2)));
    CHECK(id2 == id);
    memset(&rec, 0, sizeof(rec));
    CHECK(get_device(&d, id, &rec));
    CHECK(strcmp(rec.name, "Реле") == 0);

    CHECK(sys_ok(domain_deinit(&d)));
}

static void test_parent_and_bind(void)
{
    domain_t d;
    memset(&d, 0, sizeof(d));
    ctx_init(&d);

    const ha_device_desc_t root = {.source = TEST_SOURCE,
                                   .native_id = ROOT_NATIVE,
                                   .parent_native_id = "",
                                   .name = "Реле"};
    ha_device_id_t root_id = HA_DEVICE_ID_NONE;
    CHECK(sys_ok(device_register(&d, DOMAIN_SOURCE_UI, &root, &root_id)));

    const ha_device_desc_t child = {.source = TEST_SOURCE,
                                    .native_id = CHILD_NATIVE,
                                    .parent_native_id = ROOT_NATIVE,
                                    .name = "Канал 1"};
    ha_device_id_t child_id = HA_DEVICE_ID_NONE;
    CHECK(sys_ok(device_register(&d, DOMAIN_SOURCE_UI, &child, &child_id)));
    CHECK(child_id != root_id);

    ha_logical_device_record_t rec = {0};
    CHECK(get_device(&d, child_id, &rec));
    CHECK(rec.parent_device_id == root_id);

    /* bind: Entity должна существовать. */
    const ha_entity_id_t e1 = 0xE1ull;
    const ha_entity_id_t e2 = 0xE2ull;
    put_entity(&d, e1, "клавиша 1");

    ha_device_id_t got = HA_DEVICE_ID_NONE;
    CHECK(sys_ok(device_bind_entity(&d, DOMAIN_SOURCE_UI, root_id, e1)));
    CHECK(get_entity_device(&d, e1, &got) && got == root_id);
    CHECK(sys_ok(device_bind_entity(&d, DOMAIN_SOURCE_UI, root_id, e1))); /* idempotent */

    /* Reassign уже привязанной Entity к другому Device — отклоняется. */
    const sys_error_t reassign = device_bind_entity(&d, DOMAIN_SOURCE_UI, child_id, e1);
    CHECK(sys_is(reassign, SYS_CODE_INVALID_STATE));
    CHECK(get_entity_device(&d, e1, &got) && got == root_id);

    /* отсутствующая Entity → NOT_FOUND. */
    const sys_error_t missing =
        device_bind_entity(&d, DOMAIN_SOURCE_UI, root_id, e2);
    CHECK(sys_is(missing, SYS_CODE_NOT_FOUND));

    CHECK(sys_ok(domain_deinit(&d)));
}

static char g_removed_native[HA_NATIVE_ID_MAX];
static int g_removed_count;
static sys_error_t test_remover(const char *native_id, void *ctx)
{
    (void)ctx;
    g_removed_count++;
    snprintf(g_removed_native, sizeof(g_removed_native), "%s", native_id != NULL ? native_id : "");
    return SYS_OK;
}

/* B7: transport remove (remover по source) → cascade Entity/state/caps/group_item; automation
 * сохраняется (broken-ссылка), сам Device удаляется. */
static void test_remove_cascade(void)
{
    domain_t d;
    memset(&d, 0, sizeof(d));
    ctx_init(&d);
    CHECK(sys_ok(device_register_remover(TEST_SOURCE, test_remover, NULL)));

    const ha_device_desc_t root = {.source = TEST_SOURCE,
                                   .native_id = ROOT_NATIVE,
                                   .parent_native_id = "",
                                   .name = "Реле"};
    ha_device_id_t id = HA_DEVICE_ID_NONE;
    CHECK(sys_ok(device_register(&d, DOMAIN_SOURCE_UI, &root, &id)));

    const ha_entity_id_t e1 = 0xE1ull;
    put_entity(&d, e1, "клавиша 1");
    CHECK(sys_ok(device_bind_entity(&d, DOMAIN_SOURCE_UI, id, e1)));

    /* canonical-следы Entity */
    const ha_state_key_t ps_key = {.entity = e1, .property = HA_PROPERTY_POWER};
    const ha_state_record_t ps_rec = {.value = {.kind = HA_VALUE_BOOL}};
    domain_fact_meta_t meta = {0};
    bool changed = false;
    CHECK(sys_ok(domain_entity_put(&d, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &ps_key, &ps_rec,
                                   &meta, &changed)));
    const ha_entity_key_t ekey = {.id = e1};
    const ha_entity_capabilities_record_t caps = {0};
    CHECK(sys_ok(domain_entity_put(&d, (domain_entity_t)HA_ENTITY_CAPABILITIES, &ekey, &caps, &meta,
                                   &changed)));
    ha_group_item_key_t gi_key = {0};
    gi_key.group_id = 1;
    gi_key.entity = e1;
    const ha_group_item_record_t gi_rec = {0};
    CHECK(sys_ok(domain_entity_put(&d, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &gi_key, &gi_rec,
                                   &meta, &changed)));
    /* automation, ссылающаяся на Entity — должна сохраниться */
    const ha_automation_key_t akey = {.id = 1};
    ha_rule_t rule = {0};
    rule.enabled = 1;
    rule.trigger.kind = HA_TRIGGER_STATE;
    rule.trigger.entity = e1;
    rule.action_ref.entity = e1;
    rule.action = HA_ACTION_ON;
    CHECK(sys_ok(domain_entity_put(&d, (domain_entity_t)HA_ENTITY_AUTOMATION, &akey, &rule, &meta,
                                   &changed)));

    g_removed_count = 0;
    CHECK(sys_ok(device_remove(&d, DOMAIN_SOURCE_UI, id)));
    CHECK(g_removed_count == 1 && strcmp(g_removed_native, ROOT_NATIVE) == 0);

    ha_entity_record_t er = {0};
    ha_state_record_t psr = {0};
    ha_entity_capabilities_record_t cr = {0};
    ha_group_item_record_t gir = {0};
    ha_logical_device_record_t lr = {0};
    CHECK(!sys_ok(domain_entity_get(&d, (domain_entity_t)HA_ENTITY_ENTITY, &ekey, &er)));
    CHECK(!sys_ok(domain_entity_get(&d, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &ps_key, &psr)));
    CHECK(!sys_ok(domain_entity_get(&d, (domain_entity_t)HA_ENTITY_CAPABILITIES, &ekey, &cr)));
    CHECK(!sys_ok(domain_entity_get(&d, (domain_entity_t)HA_ENTITY_GROUP_ITEM, &gi_key, &gir)));
    CHECK(!get_device(&d, id, &lr));
    ha_rule_t kept = {0};
    CHECK(sys_ok(domain_entity_get(&d, (domain_entity_t)HA_ENTITY_AUTOMATION, &akey, &kept)));

    /* повторное удаление — Device уже нет → NOT_FOUND */
    CHECK(sys_is(device_remove(&d, DOMAIN_SOURCE_UI, id), SYS_CODE_NOT_FOUND));

    CHECK(sys_ok(domain_deinit(&d)));
}

/* B6: initial name заполняется если пусто; rename меняет только name; повторный register не перетирает. */
static void test_rename_and_initial_name(void)
{
    domain_t d;
    memset(&d, 0, sizeof(d));
    ctx_init(&d);

    const ha_device_desc_t unnamed = {.source = TEST_SOURCE,
                                      .native_id = ROOT_NATIVE,
                                      .parent_native_id = "",
                                      .name = ""};
    ha_device_id_t id = HA_DEVICE_ID_NONE;
    CHECK(sys_ok(device_register(&d, DOMAIN_SOURCE_UI, &unnamed, &id)));

    /* initial name из adapter заполняет пустое имя */
    ha_device_desc_t named = unnamed;
    snprintf(named.name, sizeof(named.name), "%s", "Реле");
    ha_device_id_t again = HA_DEVICE_ID_NONE;
    CHECK(sys_ok(device_register(&d, DOMAIN_SOURCE_UI, &named, &again)));
    CHECK(again == id);
    ha_logical_device_record_t rec = {0};
    CHECK(get_device(&d, id, &rec) && strcmp(rec.name, "Реле") == 0);

    /* rename меняет только name; source/native/parent не трогает */
    CHECK(sys_ok(device_rename(&d, DOMAIN_SOURCE_UI, id, "Кухня")));
    memset(&rec, 0, sizeof(rec));
    CHECK(get_device(&d, id, &rec));
    CHECK(strcmp(rec.name, "Кухня") == 0);
    CHECK(rec.source == TEST_SOURCE);
    CHECK(strcmp(rec.native_id, ROOT_NATIVE) == 0);
    CHECK(rec.parent_device_id == HA_DEVICE_ID_NONE);

    /* повторный register (другое имя) не перетирает пользовательское */
    ha_device_desc_t other = unnamed;
    memcpy(other.name, "Zzz", 4);
    CHECK(sys_ok(device_register(&d, DOMAIN_SOURCE_UI, &other, &again)));
    memset(&rec, 0, sizeof(rec));
    CHECK(get_device(&d, id, &rec) && strcmp(rec.name, "Кухня") == 0);

    /* rename отсутствующего Device → NOT_FOUND */
    CHECK(sys_is(device_rename(&d, DOMAIN_SOURCE_UI, 0xDEADull, "x"), SYS_CODE_NOT_FOUND));

    CHECK(sys_ok(domain_deinit(&d)));
}

int main(void)
{
    test_register_and_idempotent();
    test_parent_and_bind();
    test_remove_cascade();
    test_rename_and_initial_name();

    if (g_failures == 0) {
        printf("all device-service tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", g_failures);
    return 1;
}
