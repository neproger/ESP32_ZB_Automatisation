#pragma once

/*
 * Форма погоды (docs/services/SYSTEM.md): текущие условия одним record'ом, аналогично
 * location системного устройства. Форма, а не логика: строк нет — условие кодируется
 * enum, числа фиксированной ширины. Заполняет system-сервис (Open-Meteo); Display и
 * Automation читают как обычную сущность.
 *
 * Источник данных v1 — Open-Meteo (current: temperature_2m, relative_humidity_2m,
 * weather_code, wind_speed_10m). Сервис переводит WMO weather_code в condition.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UID синтетической сущности погоды: вне диапазона Zigbee (не EUI-64 с реальным OUI),
 * ASCII "WTHR" в старших байтах — как HA_SYSTEM_DEVICE_UID для времени.
 */
#define HA_WEATHER_DEVICE_UID 0x5754485200000001ull

/*
 * Условие погоды. Имена совпадают с наборами Home Assistant / иконками v1
 * (sunny, partlycloudy, ... exceptional). Значения — стабильный дискриминатор.
 */
typedef enum {
    HA_WEATHER_UNKNOWN = 0,
    HA_WEATHER_CLEAR = 1,          /* ясно (день) */
    HA_WEATHER_CLEAR_NIGHT = 2,    /* ясно (ночь) */
    HA_WEATHER_PARTLYCLOUDY = 3,
    HA_WEATHER_CLOUDY = 4,
    HA_WEATHER_OVERCAST = 5,
    HA_WEATHER_FOG = 6,
    HA_WEATHER_RAINY = 7,
    HA_WEATHER_POURING = 8,
    HA_WEATHER_SNOWY = 9,
    HA_WEATHER_SNOWY_RAINY = 10,
    HA_WEATHER_HAIL = 11,
    HA_WEATHER_LIGHTNING = 12,
    HA_WEATHER_LIGHTNING_RAINY = 13,
    HA_WEATHER_WINDY = 14,
    HA_WEATHER_WINDY_VARIANT = 15,
    HA_WEATHER_EXCEPTIONAL = 16,
} ha_weather_condition_t;

/*
 * Текущие условия. Масштабы как у ZCL: 0.01 °C и 0.01 %, чтобы читатель не гадал.
 * Временных меток нет: они operational metadata и ломали бы свёртку (RECORD_MODEL §2.2);
 * факт «обновлено» приходит отдельным событием сервиса.
 */
typedef struct {
    uint8_t condition;         /* ha_weather_condition_t */
    uint8_t cloud_pct;         /* 0..100 */
    int16_t temperature_c100;  /* 0.01 °C */
    uint16_t humidity_p100;    /* 0.01 % */
    uint16_t pressure_hpa;     /* гПа */
    uint16_t wind_kmh10;       /* 0.1 км/ч */
    uint16_t wind_dir_deg;     /* 0..359 */
    uint8_t reserved[4];
} ha_weather_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_weather_record_t) == 16, "ha_weather_record_t: неожиданный размер");
static_assert(offsetof(ha_weather_record_t, temperature_c100) == 2, "weather: layout");
static_assert(offsetof(ha_weather_record_t, humidity_p100) == 4, "weather: layout");
#else
_Static_assert(sizeof(ha_weather_record_t) == 16, "ha_weather_record_t: неожиданный размер");
_Static_assert(offsetof(ha_weather_record_t, temperature_c100) == 2, "weather: layout");
_Static_assert(offsetof(ha_weather_record_t, humidity_p100) == 4, "weather: layout");
#endif

#ifdef __cplusplus
}
#endif
