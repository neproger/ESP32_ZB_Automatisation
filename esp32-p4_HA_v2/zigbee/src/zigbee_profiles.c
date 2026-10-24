#include "zigbee/zigbee_profiles.h"

#include <stddef.h>
#include <string.h>

#include "ha_model/ha_properties.h"
#include "zigbee/generated/zb_catalog_generated.h"

/*
 * Ручные записи до появления генератора. Порядок важен: первое совпадение
 * выигрывает, поэтому узкие записи идут выше общих.
 */

/* TS0041 (Tuya): кнопка одна, endpoint'ы 2..4 — фантомные; событие приходит
 * командой OnOff 0xFD на ep1 (0/1/2 → single/double/hold). */
static const uint8_t k_ts0041_properties[] = {HA_PROPERTY_BATTERY_PERCENT,
                                              HA_PROPERTY_BATTERY_VOLTAGE};
static const uint8_t k_ts0041_events[] = {HA_EVENT_SINGLE_PRESS, HA_EVENT_DOUBLE_PRESS,
                                          HA_EVENT_HOLD};

static const zb_capability_profile_t k_ts0041_capability = {
    .properties = k_ts0041_properties,
    .property_count = sizeof(k_ts0041_properties) / sizeof(k_ts0041_properties[0]),
    .events = k_ts0041_events,
    .event_count = sizeof(k_ts0041_events) / sizeof(k_ts0041_events[0]),
    .button_endpoint = 1,
    .drop_power = true,
};

static const zb_protocol_binding_t k_tuya_onoff_action = {
    .kind = ZB_PROTOCOL_TUYA_ONOFF_ACTION,
    .button_endpoint = 1,
};

static const zb_device_profile_t k_profiles[] = {
    {
        .manufacturer = "_TZ3000",
        .model = "TS0041",
        .capability = &k_ts0041_capability,
        .protocol = &k_tuya_onoff_action,
    },
};

#define PROFILE_COUNT (sizeof(k_profiles) / sizeof(k_profiles[0]))

static bool protocol_supported(zb_protocol_kind_t kind)
{
    return kind == ZB_PROTOCOL_STANDARD || kind == ZB_PROTOCOL_TUYA_ONOFF_ACTION;
}

static bool field_matches(const char *value, const char *needle)
{
    return needle == NULL || (value != NULL && strstr(value, needle) != NULL);
}

static zb_profile_match_t match_profile(const zb_device_profile_t *profile,
                                        const zb_device_signature_t *signature,
                                        const zb_device_profile_t **out)
{
    if (!field_matches(signature->manufacturer, profile->manufacturer) ||
        !field_matches(signature->model, profile->model)) {
        return ZB_PROFILE_NONE;
    }
    if (out != NULL) {
        *out = profile;
    }
    if (profile->protocol != NULL && !protocol_supported(profile->protocol->kind)) {
        return ZB_PROFILE_RECOGNIZED;
    }
    return ZB_PROFILE_SUPPORTED;
}

zb_profile_match_t zb_profiles_match(const zb_device_signature_t *signature,
                                     const zb_device_profile_t **out)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (signature == NULL) {
        return ZB_PROFILE_NONE;
    }
    /* Ручные записи выигрывают у сгенерированных: они уточняют поведение. */
    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        const zb_profile_match_t match = match_profile(&k_profiles[i], signature, out);
        if (match != ZB_PROFILE_NONE) {
            return match;
        }
    }
    for (size_t i = 0; i < zb_catalog_generated_count; i++) {
        const zb_profile_match_t match =
            match_profile(&zb_catalog_generated[i], signature, out);
        if (match != ZB_PROFILE_NONE) {
            return match;
        }
    }
    return ZB_PROFILE_NONE;
}
