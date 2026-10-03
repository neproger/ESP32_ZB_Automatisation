#include "zigbee/zigbee_state.h"

#include <string.h>

#include "sys/sys_error.h"

/*
 * Репорт → состояние. Ничего не знает о радио, очередях и задачах: этим занимается
 * zigbee.c. Здесь только форма записи и правило появления устройства: устройство
 * появляется по завершении интервью (zigbee_interview_apply), не по репорту.
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

sys_error_t zigbee_device_set_model(domain_t *domain, ha_device_uid_t uid, const char *model,
                                    bool *out_changed)
{
    if (domain == NULL || model == NULL || out_changed == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    *out_changed = false;

    /* Имя задаёт пользователь: читаем существующую запись, чтобы её не затереть. */
    ha_device_record_t record = {0};
    const sys_error_t found =
        domain_entity_get(domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &record);
    if (sys_failed(found) && !sys_is(found, SYS_CODE_NOT_FOUND)) {
        return found;
    }

    size_t length = strlen(model);
    if (length >= sizeof(record.model)) {
        length = sizeof(record.model) - 1; /* обрезаем: поле фиксированной длины */
    }
    memcpy(record.model, model, length);
    record.model[length] = '\0';

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;

    return domain_entity_put(domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &record, &meta,
                             out_changed);
}

/* Создать запись устройства, если её ещё нет: сюда приходит топология интервью. */
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

/* Поиск первой записи типа с нужным uid: ключ копируется, iter прерывается. */
typedef struct {
    ha_device_uid_t uid;
    uint8_t *key_out;
    size_t key_size;
    bool found;
} first_key_ctx_t;

static bool find_first_key(const void *key, const void *record, void *ctx)
{
    (void)record;
    first_key_ctx_t *find = (first_key_ctx_t *)ctx;
    if (memcmp(key, &find->uid, sizeof(find->uid)) != 0) {
        return true;
    }
    memcpy(find->key_out, key, find->key_size);
    find->found = true;
    return false;
}

/* Удаление записей типа по одному: мутировать Domain внутри iter нельзя. */
static sys_error_t remove_device_entities(domain_t *domain, domain_entity_t type, size_t key_size,
                                          ha_device_uid_t uid)
{
    uint8_t key[sizeof(ha_zb_state_key_t)];
    if (key_size > sizeof(key)) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    for (;;) {
        first_key_ctx_t find = {.uid = uid, .key_out = key, .key_size = key_size, .found = false};
        const sys_error_t iter = domain_entity_iter(domain, type, find_first_key, &find);
        if (sys_failed(iter)) {
            return iter;
        }
        if (!find.found) {
            return SYS_OK;
        }

        domain_fact_meta_t meta = {0};
        meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
        const sys_error_t removed = domain_entity_remove(domain, type, key, &meta);
        if (sys_failed(removed)) {
            return removed;
        }
    }
}

sys_error_t zigbee_device_remove(domain_t *domain, ha_device_uid_t uid)
{
    if (domain == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    sys_error_t err = remove_device_entities(domain, (domain_entity_t)HA_ENTITY_STATE,
                                             sizeof(ha_zb_state_key_t), uid);
    if (sys_failed(err)) {
        return err;
    }
    err = remove_device_entities(domain, (domain_entity_t)HA_ENTITY_ENDPOINT,
                                 sizeof(ha_endpoint_key_t), uid);
    if (sys_failed(err)) {
        return err;
    }

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_ZIGBEE;
    err = domain_entity_remove(domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &meta);
    if (sys_failed(err) && !sys_is(err, SYS_CODE_NOT_FOUND)) {
        return err;
    }
    return SYS_OK;
}

sys_error_t zigbee_state_apply(domain_t *domain, const zigbee_report_t *report, bool *out_changed)
{
    if (domain == NULL || report == NULL || out_changed == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    *out_changed = false;

    /* Устройства без интервью в Domain нет: репорт от него — не факт (§9.4). */
    ha_device_record_t device = {0};
    const sys_error_t known =
        domain_entity_get(domain, (domain_entity_t)HA_ENTITY_DEVICE, &report->device_uid, &device);
    if (sys_failed(known)) {
        return known;
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
