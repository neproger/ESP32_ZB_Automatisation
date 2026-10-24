#pragma once

/*
 * Физические Zigbee-формы (Шаг C): transport vocabulary моста `semantics`. Раньше
 * лежали в ha_model/ha_entities.h, но generic Domain их не знает и знать не должен.
 *
 * Здесь только формы (структуры и числа), никакой логики. Ключ состояния — числовой
 * ZCL-ключ (RECORD_MODEL §10.2); топология — состав кластеров endpoint'а.
 */

#include <stddef.h>
#include <stdint.h>

#include "zigbee/ha_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HA_DEVICE_NAME_MAX 32
#define HA_DEVICE_MODEL_MAX 32

/*
 * Устройство адаптера: имя (задаёт пользователь) + модель (Basic-кластер). Adapter-private
 * (Zigbee RAM store), в generic Domain не хранится.
 */
typedef struct {
    char name[HA_DEVICE_NAME_MAX];
    char manufacturer[HA_DEVICE_NAME_MAX]; /* Basic 0x0004: идентификация вендора/профиля */
    char model[HA_DEVICE_MODEL_MAX];
} ha_device_record_t;

/*
 * Ключ состояния — та же координата, что в ZCL-репорте, без промежуточного
 * семантического отображения (docs/services/ZIGBEE.md §9.1).
 */
typedef struct {
    uint64_t device_uid;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t endpoint;
    uint8_t reserved[3];
} ha_zb_state_key_t;

/*
 * Состояние одного атрибута: значение в кодировке ZCL с явным типом. Масштабирование
 * (0.01 °C, уровень 0..254) — сторона читателя (ha_zigbee.h).
 */
typedef struct {
    uint32_t raw;
    uint8_t zcl_type;
    uint8_t reserved[3];
} ha_zb_state_record_t;

/* Endpoint — топология устройства: состав кластеров и их роли из Simple Descriptor. */
#define HA_ENDPOINT_CLUSTERS_MAX 16

typedef struct {
    uint16_t cluster_id; /* из ha_zigbee.h */
    uint8_t role;        /* HA_ZB_ROLE_CLIENT | HA_ZB_ROLE_SERVER */
    uint8_t reserved;
} ha_cluster_entry_t;

typedef struct {
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t cluster_count;
    uint8_t reserved[3];
    ha_cluster_entry_t clusters[HA_ENDPOINT_CLUSTERS_MAX];
} ha_endpoint_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_zb_state_key_t) == 16, "ha_zb_state_key_t: неожиданный размер");
static_assert(offsetof(ha_zb_state_key_t, cluster_id) == 8, "ha_zb_state_key_t: layout");
static_assert(offsetof(ha_zb_state_key_t, attr_id) == 10, "ha_zb_state_key_t: layout");
static_assert(offsetof(ha_zb_state_key_t, endpoint) == 12, "ha_zb_state_key_t: layout");
static_assert(sizeof(ha_zb_state_record_t) == 8, "ha_zb_state_record_t: неожиданный размер");
static_assert(sizeof(ha_device_record_t) == 96, "ha_device_record_t: неожиданный размер");
static_assert(sizeof(ha_cluster_entry_t) == 4, "ha_cluster_entry_t: неожиданный размер");
static_assert(sizeof(ha_endpoint_record_t) == 72, "ha_endpoint_record_t: неожиданный размер");
#else
_Static_assert(sizeof(ha_zb_state_key_t) == 16, "ha_zb_state_key_t: неожиданный размер");
_Static_assert(offsetof(ha_zb_state_key_t, cluster_id) == 8, "ha_zb_state_key_t: layout");
_Static_assert(offsetof(ha_zb_state_key_t, attr_id) == 10, "ha_zb_state_key_t: layout");
_Static_assert(offsetof(ha_zb_state_key_t, endpoint) == 12, "ha_zb_state_key_t: layout");
_Static_assert(sizeof(ha_zb_state_record_t) == 8, "ha_zb_state_record_t: неожиданный размер");
_Static_assert(sizeof(ha_device_record_t) == 96, "ha_device_record_t: неожиданный размер");
_Static_assert(sizeof(ha_cluster_entry_t) == 4, "ha_cluster_entry_t: неожиданный размер");
_Static_assert(sizeof(ha_endpoint_record_t) == 72, "ha_endpoint_record_t: неожиданный размер");
#endif

#ifdef __cplusplus
}
#endif
