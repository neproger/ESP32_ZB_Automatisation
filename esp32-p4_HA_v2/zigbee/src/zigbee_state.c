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
