#include "zigbee/zigbee_entity_registry.h"

#include "esp_log.h"
#include "zigbee/zigbee_entity_binding.h"

static const char *TAG = "zb_entity";

#define ZB_ENTITY_BINDING_MAX 32

/* In-RAM кэш привязок. Identity детерминирована (A0.3) → переживает reboot без persistence. */
static zb_entity_binding_t s_bindings[ZB_ENTITY_BINDING_MAX];
static size_t s_binding_count;

sys_error_t zigbee_entity_ensure(domain_t *domain, ha_device_uid_t uid, uint8_t endpoint,
                                 ha_entity_id_t *out_entity_id)
{
    if (domain == NULL || out_entity_id == NULL) {
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_INVALID_ARG);
    }

    ha_entity_id_t id = HA_ENTITY_ID_NONE;
    bool created = false;
    if (!zb_entity_resolve(s_bindings, ZB_ENTITY_BINDING_MAX, &s_binding_count, uid, endpoint, &id,
                           &created)) {
        ESP_LOGW(TAG, "entity resolve failed uid=%llx ep=%u", (unsigned long long)uid,
                 (unsigned)endpoint);
        return sys_error_make(SYS_LAYER_ZIGBEE, SYS_CODE_NO_MEM);
    }

    const ha_entity_key_t key = {.id = id};
    const ha_entity_record_t record = {0};
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    bool changed = false;
    const sys_error_t err =
        domain_entity_put(domain, (domain_entity_t)HA_ENTITY_ENTITY, &key, &record, &meta, &changed);
    if (sys_failed(err)) {
        ESP_LOGW(TAG, "entity put failed id=%llx err=%u", (unsigned long long)id,
                 (unsigned)err.code);
        return err;
    }
    if (created) {
        ESP_LOGI(TAG, "entity bound uid=%llx ep=%u id=%llx", (unsigned long long)uid,
                 (unsigned)endpoint, (unsigned long long)id);
    }
    *out_entity_id = id;
    return SYS_OK;
}
