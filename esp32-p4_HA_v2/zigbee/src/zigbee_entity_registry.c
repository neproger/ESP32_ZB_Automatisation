#include "zigbee/zigbee_entity_registry.h"

#include <string.h>

#include "esp_log.h"
#include "zigbee/zigbee_entity_binding.h"

static const char *TAG = "zb_entity";

#define ZB_ENTITY_BINDING_MAX 32

/* In-RAM кэш привязок. Identity детерминирована (A0.3) → переживает reboot без persistence. */
static zb_entity_binding_t s_bindings[ZB_ENTITY_BINDING_MAX];
static size_t s_binding_count;

sys_error_t zigbee_entity_ensure(domain_t *domain, ha_device_uid_t uid, uint8_t endpoint,
                                 uint8_t endpoint_total, ha_entity_id_t *out_entity_id)
{
    char initial_name[sizeof(((ha_entity_record_t *)0)->name)] = {0};
    zb_entity_initial_name(domain, uid, endpoint, endpoint_total, initial_name,
                           sizeof(initial_name));

    const size_t before = s_binding_count;
    const sys_error_t err = zb_entity_ensure_entity(
        domain, s_bindings, ZB_ENTITY_BINDING_MAX, &s_binding_count, uid, endpoint,
        initial_name[0] != '\0' ? initial_name : NULL, out_entity_id);
    if (sys_failed(err)) {
        ESP_LOGW(TAG, "entity ensure failed uid=%llx ep=%u err=%u", (unsigned long long)uid,
                 (unsigned)endpoint, (unsigned)err.code);
        return err;
    }
    if (s_binding_count > before) {
        ESP_LOGI(TAG, "entity bound uid=%llx ep=%u id=%llx", (unsigned long long)uid,
                 (unsigned)endpoint, (unsigned long long)*out_entity_id);
    }
    return SYS_OK;
}

bool zigbee_entity_lookup(ha_device_uid_t uid, uint8_t endpoint, ha_entity_id_t *out_entity_id)
{
    if (out_entity_id == NULL) {
        return false;
    }
    return zb_entity_binding_find_entity(s_bindings, s_binding_count, uid, endpoint,
                                         out_entity_id);
}

bool zigbee_entity_physical(ha_entity_id_t entity_id, ha_device_uid_t *out_uid,
                            uint8_t *out_endpoint)
{
    if (out_uid == NULL || out_endpoint == NULL) {
        return false;
    }
    return zb_entity_binding_find_physical(s_bindings, s_binding_count, entity_id, out_uid,
                                           out_endpoint);
}

void zigbee_entity_forget(ha_device_uid_t uid)
{
    if (zb_entity_binding_remove_uid(s_bindings, &s_binding_count, uid)) {
        ESP_LOGI(TAG, "entity bindings forgotten uid=%llx", (unsigned long long)uid);
    }
}
