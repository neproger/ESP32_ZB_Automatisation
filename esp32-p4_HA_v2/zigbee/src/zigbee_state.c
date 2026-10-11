#include "zigbee/zigbee_state.h"

#include <string.h>

#include "semantics/semantics.h"

/*
 * Репорт → canonical состояние. Ничего не знает о радио, очередях и задачах: этим
 * занимается zigbee.c. Здесь только проекция физического репорта в семантическое
 * значение (entity_id, property_id).
 *
 * Raw-состояние как diagnostics-хранилище удалено (Шаг C): истина состояния —
 * canonical HA_ENTITY_PROPERTY_STATE.
 */

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

bool zigbee_property_state_from_report(const zigbee_report_t *report, ha_entity_id_t entity_id,
                                       ha_state_key_t *out_key, ha_state_record_t *out_record)
{
    if (report == NULL || out_key == NULL || out_record == NULL || entity_id == HA_ENTITY_ID_NONE) {
        return false;
    }

    ha_zb_state_key_t physical = {0};
    zigbee_state_key_build(report, &physical);

    const ha_property_id_t property = semantics_property_from_key(&physical);
    if (property == HA_PROPERTY_UNKNOWN) {
        return false;
    }

    const ha_zb_state_record_t raw = {.raw = report->raw, .zcl_type = report->zcl_type};
    ha_value_t value = {0};
    if (!semantics_state_value(&physical, &raw, &value)) {
        return false;
    }

    *out_key = (ha_state_key_t){0}; /* детерминированный ключ: reserved = 0 */
    out_key->entity = entity_id;
    out_key->property = property;
    out_record->value = value;
    return true;
}
