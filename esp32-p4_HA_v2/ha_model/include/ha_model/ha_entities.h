#pragma once

/*
 * Формы сущностей Entity Store (docs/RECORD_MODEL.md §1, §11).
 *
 * Формы, а не логика: только структуры и числа, никаких зависимостей. Заголовок
 * линкуют сервисы и bootstrap; Domain его не включает и этих имён не знает.
 *
 * Здесь зафиксированы два решения Zigbee-слоя:
 *   - ключ состояния — числовой ZCL-ключ (docs/RECORD_MODEL.md §10.2);
 *   - runtime state — одна сущность на один атрибут (docs/services/ZIGBEE.md §9).
 *
 * Чего здесь нет: operational metadata (last_seen, rssi, lqi) — она меняется на
 * каждом сообщении и потому в canonical state не входит (RECORD_MODEL.md:123-138).
 * Топология (endpoint, cluster list) — отдельный шаг вместе с интервью; у устройства
 * она не меняется, поэтому endpoint хранится во flash (bootstrap: FLASH + persist_key),
 * а state атрибутов — только RAM (репорты приносят его заново).
 */

#include <stddef.h>
#include <stdint.h>

#include "ha_model/ha_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Дискриминатор типа сущности: внутренняя runtime identity Domain, а не wire ABI —
 * наружу тип передаётся строковым именем (docs/RECORD_MODEL.md:60-63).
 */
typedef enum {
    HA_ENTITY_DEVICE = 1,
    HA_ENTITY_STATE = 2,
    HA_ENTITY_ENDPOINT = 3,
    HA_ENTITY_AUTOMATION = 4,
    /* Список устройств на удаление: ключ — uid. Устройство всё равно нужно удалить из
     * сети, поэтому держим пометку, пока не пройдёт leave (docs/services/ZIGBEE.md). */
    HA_ENTITY_DEVICE_REMOVE = 5,
} ha_entity_t;

#define HA_DEVICE_NAME_MAX 32
#define HA_DEVICE_MODEL_MAX 32

/*
 * Устройство. Ключ — сам ha_device_uid_t (EUI-64), отдельной структуры под ключ нет.
 * Имя задаёт пользователь, модель приходит из Basic-кластера.
 */
typedef struct {
    char name[HA_DEVICE_NAME_MAX];
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
 * Состояние одного атрибута. Единая форма обязательна: payload_size фиксирован на
 * тип сущности, поэтому значение хранится в кодировке ZCL с явным типом.
 * Масштабирование (0.01 °C, уровень 0..254) — сторона читателя, а не записи
 * (ha_zigbee.h:15-16).
 */
typedef struct {
    uint32_t raw;
    uint8_t zcl_type;
    uint8_t reserved[3];
} ha_zb_state_record_t;

/*
 * Endpoint — топология устройства: чем оно умеет быть. Состояние живёт отдельно, на
 * атрибут; здесь только состав кластеров и их роли из Simple Descriptor.
 *
 * Номер endpoint'а лежит в ключе и в записи не дублируется.
 */
#define HA_ENDPOINT_CLUSTERS_MAX 16

typedef struct {
    uint16_t cluster_id; /* из ha_zigbee.h */
    uint8_t role;        /* HA_ZB_ROLE_CLIENT | HA_ZB_ROLE_SERVER */
    uint8_t reserved;
} ha_cluster_entry_t;

typedef struct {
    uint64_t device_uid;
    uint8_t endpoint;
    uint8_t reserved[7];
} ha_endpoint_key_t;

typedef struct {
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t cluster_count;
    uint8_t reserved[3];
    ha_cluster_entry_t clusters[HA_ENDPOINT_CLUSTERS_MAX];
} ha_endpoint_record_t;

/* Пометка «устройство на удаление». Ключ — сам ha_device_uid_t. */
typedef struct {
    uint8_t requested;
    uint8_t reserved[7];
} ha_device_remove_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_zb_state_key_t) == 16, "ha_zb_state_key_t: неожиданный размер");
static_assert(offsetof(ha_zb_state_key_t, cluster_id) == 8, "ha_zb_state_key_t: layout");
static_assert(offsetof(ha_zb_state_key_t, attr_id) == 10, "ha_zb_state_key_t: layout");
static_assert(offsetof(ha_zb_state_key_t, endpoint) == 12, "ha_zb_state_key_t: layout");
static_assert(sizeof(ha_zb_state_record_t) == 8, "ha_zb_state_record_t: неожиданный размер");
static_assert(sizeof(ha_device_record_t) == 64, "ha_device_record_t: неожиданный размер");
static_assert(sizeof(ha_cluster_entry_t) == 4, "ha_cluster_entry_t: неожиданный размер");
static_assert(sizeof(ha_endpoint_key_t) == 16, "ha_endpoint_key_t: неожиданный размер");
static_assert(offsetof(ha_endpoint_key_t, endpoint) == 8, "ha_endpoint_key_t: layout");
static_assert(sizeof(ha_endpoint_record_t) == 72, "ha_endpoint_record_t: неожиданный размер");
static_assert(sizeof(ha_device_remove_record_t) == 8, "ha_device_remove_record_t: неожиданный размер");
#else
_Static_assert(sizeof(ha_zb_state_key_t) == 16, "ha_zb_state_key_t: неожиданный размер");
_Static_assert(offsetof(ha_zb_state_key_t, cluster_id) == 8, "ha_zb_state_key_t: layout");
_Static_assert(offsetof(ha_zb_state_key_t, attr_id) == 10, "ha_zb_state_key_t: layout");
_Static_assert(offsetof(ha_zb_state_key_t, endpoint) == 12, "ha_zb_state_key_t: layout");
_Static_assert(sizeof(ha_zb_state_record_t) == 8, "ha_zb_state_record_t: неожиданный размер");
_Static_assert(sizeof(ha_device_record_t) == 64, "ha_device_record_t: неожиданный размер");
_Static_assert(sizeof(ha_cluster_entry_t) == 4, "ha_cluster_entry_t: неожиданный размер");
_Static_assert(sizeof(ha_endpoint_key_t) == 16, "ha_endpoint_key_t: неожиданный размер");
_Static_assert(offsetof(ha_endpoint_key_t, endpoint) == 8, "ha_endpoint_key_t: layout");
_Static_assert(sizeof(ha_endpoint_record_t) == 72, "ha_endpoint_record_t: неожиданный размер");
_Static_assert(sizeof(ha_device_remove_record_t) == 8, "ha_device_remove_record_t: неожиданный размер");
#endif

#ifdef __cplusplus
}
#endif
