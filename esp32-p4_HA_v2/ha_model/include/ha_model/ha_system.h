#pragma once

/*
 * Идентификатор системного устройства (docs/services/SYSTEM.md): время (позже — погода).
 *
 * Это не Zigbee: устройство синтетическое. Семантику своих величин system публикует
 * напрямую в canonical PROPERTY_STATE (ha_properties.h), поэтому здесь — только id.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Id системного устройства — вне диапазона Zigbee (не EUI-64 с реальным OUI), чтобы
 * заведомо не столкнуться с реальным устройством. ASCII "SYST" в старших байтах.
 */
#define HA_SYSTEM_DEVICE_ID 0x53595354454D0001ull

/*
 * События системного устройства публикуются фактом EVENT (value.enum) в ОБЩЕМ vocabulary
 * `ha_event_id_t` (ha_properties.h): HA_EVENT_MINUTE_TICK и т.д. Automation вешает правила
 * на пару (entity, event_id).
 */

#ifdef __cplusplus
}
#endif
