#pragma once

/*
 * Словарь системного устройства (docs/services/SYSTEM.md): время (позже — погода).
 *
 * Это не Zigbee: устройство синтетическое, но живёт в тех же сущностях Domain
 * (device/endpoint/state/location), поэтому Automation и Web видят его как обычное.
 * Здесь только идентификаторы и формы — никакой логики.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UID системного устройства — вне диапазона Zigbee (не EUI-64 с реальным OUI), чтобы
 * заведомо не столкнуться с реальным устройством. ASCII "SYST" в старших байтах.
 */
#define HA_SYSTEM_DEVICE_UID 0x53595354454D0001ull

#define HA_SYSTEM_ENDPOINT 1

/* Кластер времени ZCL (не путать с Analog Input 0x000C). */
#define HA_CLUSTER_TIME 0x000Au
#define HA_TIME_ATTR_UTC_TIME 0x0000u   /* u32, секунды, значение "время синхронизировано" */
#define HA_TIME_ATTR_TIME_STATUS 0x0001u /* u8, битмап; бит 1 (0x02) — time master */

/* Vendor-кластер локального времени. */
#define HA_CLUSTER_SYSTEM 0xFC00u
#define HA_SYS_ATTR_HOUR 0x0000u          /* u8, локальный час 0..23 */
#define HA_SYS_ATTR_MINUTE 0x0001u        /* u8, локальная минута 0..59 */
#define HA_SYS_ATTR_WEEKDAY 0x0002u       /* u8, 0=Пн .. 6=Вс */
#define HA_SYS_ATTR_WEEKDAY_MASK 0x0003u  /* u8, бит 0=Пн .. бит 6=Вс */
#define HA_SYS_ATTR_MINUTES_OF_DAY 0x0004u /* u16, 0..1439 (hour*60+minute) */
#define HA_SYS_ATTR_TZ_OFFSET_MIN 0x0005u /* i16, смещение локального пояса от UTC, минуты */

/*
 * События системного устройства. Публикуются фактом EVENT (domain_payload_put) с
 * value.enum = id; Automation вешает правила на пару (uid, id). Точное время задаётся
 * условием по MINUTES_OF_DAY, дни недели — условием по WEEKDAY_MASK (оператор битов).
 */
typedef enum {
    HA_SYS_EVENT_MINUTE_TICK = 1,     /* каждую минуту */
    HA_SYS_EVENT_HALF_HOUR_TICK = 2,  /* каждые 30 минут (:00 и :30) */
    HA_SYS_EVENT_HOUR_TICK = 3,       /* каждый час на :00 */
    HA_SYS_EVENT_DAY_TICK = 4,        /* 00:00 локального времени */
    HA_SYS_EVENT_WEATHER_CHANGED = 5, /* сменилось условие погоды */
} ha_system_event_t;

#ifdef __cplusplus
}
#endif
