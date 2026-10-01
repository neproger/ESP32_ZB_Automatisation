#include "zigbee/zigbee_state.h"

#include <string.h>

#include "sys/sys_error.h"

/*
 * Репорт → состояние. Ничего не знает о радио, очередях и задачах: этим занимается
 * zigbee.c. Здесь только форма записи и правило появления устройства.
 */

static sys_error_t zigbee_fail(sys_code_t code)
{
    return sys_error_make(SYS_LAYER_ZIGBEE, code);
}

void zigbee_state_key_build(const zigbee_report_t *report, ha_zb_state_key_t *out_key)
{
    ha_zb_state_key_t key = {0};
    key.device_uid = report->device_uid;
    key.cluster_id = report->cluster_id;
    key.attr_id = report->attr_id;
    key.endpoint = report->endpoint;
    *out_key = key;
}

domain_value_t zigbee_value_compact(const zigbee_report_t *report)
{
    domain_value_t value = {0};

    switch (report->zcl_type) {
    case HA_ZB_TYPE_BOOL:
    case HA_ZB_TYPE_BITMAP8:
    case HA_ZB_TYPE_UINT8:
    case HA_ZB_TYPE_UINT16:
    case HA_ZB_TYPE_UINT32:
        value.type = (uint8_t)DOMAIN_VALUE_U32;
        value.v.u32 = report->raw;
        break;
    case HA_ZB_TYPE_INT8:
    case HA_ZB_TYPE_INT16:
    case HA_ZB_TYPE_INT32:
        value.type = (uint8_t)DOMAIN_VALUE_I32;
        value.v.i32 = (int32_t)report->raw; /* значение приходит расширенным по знаку */
        break;
    case HA_ZB_TYPE_SINGLE_FLOAT: {
        float as_float = 0.0f;
        memcpy(&as_float, &report->raw, sizeof(as_float));
        value.type = (uint8_t)DOMAIN_VALUE_F32;
        value.v.f32 = as_float;
        break;
    }
    case HA_ZB_TYPE_ENUM8:
    case HA_ZB_TYPE_ENUM16:
        value.type = (uint8_t)DOMAIN_VALUE_ENUM;
        value.v.u32 = report->raw;
        break;
    default:
        value.type = (uint8_t)DOMAIN_VALUE_NONE;
        break;
    }

    return value;
}

/* Устройство появляется в хранилище вместе с первым репортом: Zigbee его уже назвал. */
sys_error_t zigbee_device_ensure(domain_t *domain, ha_device_uid_t uid)
{
    ha_device_record_t existing = {0};
    const sys_error_t found =
        domain_entity_get(domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &existing);
    if (sys_ok(found) || !sys_is(found, SYS_CODE_NOT_FOUND)) {
        return found;
    }

    ha_device_record_t record = {0}; /* имя и модель приходят позже: из Basic и от UI */
    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;

    bool changed = false;
    return domain_entity_put(domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &record, &meta,
                             &changed);
}

sys_error_t zigbee_state_apply(domain_t *domain, const zigbee_report_t *report, bool *out_changed)
{
    if (domain == NULL || report == NULL || out_changed == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    *out_changed = false;

    const sys_error_t device = zigbee_device_ensure(domain, report->device_uid);
    if (sys_failed(device)) {
        return device;
    }

    ha_zb_state_key_t key = {0};
    zigbee_state_key_build(report, &key);

    ha_zb_state_record_t record = {0};
    record.raw = report->raw;
    record.zcl_type = report->zcl_type;

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    meta.value = zigbee_value_compact(report);

    return domain_entity_put(domain, (domain_entity_t)HA_ENTITY_STATE, &key, &record, &meta,
                             out_changed);
}
