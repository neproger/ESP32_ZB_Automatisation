#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Каталог профилей Zigbee-устройств.
 *
 * Граница: каталог ОПИСЫВАЕТ устройство (capability — что доступно приложению),
 * handler ОПИСЫВАЕТ поведение (protocol — как читать/писать/декодировать), а
 * состояние живёт в Domain (обработчики публикуют факты, не пишут состояние сами).
 *
 * Пока таблица ручная; генератор каталога добавляет записи в тот же формат.
 * Сохранять profile_id как идентичность устройства нельзя: fingerprint —
 * стабилен, индекс таблицы — нет.
 */

typedef struct {
    const char *manufacturer; /* подстрока Basic 0x0004; NULL — любой */
    const char *model;        /* подстрока Basic 0x0005; NULL — любая */
} zb_device_signature_t;

/* ЧТО доступно приложению. id — canonical из ha_model. */
typedef struct {
    const uint8_t *properties; /* ha_property_id_t; NULL — вывести из кластеров */
    uint8_t property_count;
    const uint8_t *events;     /* ha_event_id_t; NULL — вывести из кластеров */
    uint8_t event_count;
    uint8_t button_endpoint;   /* 0 — все endpoint'ы; иначе только этот */
    bool drop_power;           /* кнопка не управляет питанием */
} zb_capability_profile_t;

/* КАК читать/писать/декодировать. */
typedef enum {
    ZB_PROTOCOL_STANDARD = 0,      /* ZCL: стандартные кластеры */
    ZB_PROTOCOL_TUYA_ONOFF_ACTION, /* OnOff 0xFD: payload 0/1/2 → single/double/hold */
} zb_protocol_kind_t;

typedef struct {
    zb_protocol_kind_t kind;
    uint8_t button_endpoint;
} zb_protocol_binding_t;

typedef struct {
    const char *manufacturer;
    const char *model;
    const zb_capability_profile_t *capability; /* NULL — стандартный вывод из кластеров */
    const zb_protocol_binding_t *protocol;     /* NULL — стандартный ZCL */
} zb_device_profile_t;

typedef enum {
    ZB_PROFILE_NONE = 0,   /* fingerprint не найден */
    ZB_PROFILE_RECOGNIZED, /* найден, но handler ещё не реализован */
    ZB_PROFILE_SUPPORTED,  /* найден и handler реализован */
} zb_profile_match_t;

/* Первое совпадение по manufacturer/model (подстроки). */
zb_profile_match_t zb_profiles_match(const zb_device_signature_t *signature,
                                     const zb_device_profile_t **out);
