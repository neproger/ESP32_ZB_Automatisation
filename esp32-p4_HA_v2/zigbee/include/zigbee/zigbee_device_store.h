#pragma once

/*
 * Zigbee-private RAM-модель устройства: имя/модель и топология endpoint'ов. Нужна
 * адаптеру, чтобы маршрутизировать команды, выводить canonical capabilities и давать
 * Entity начальное имя. Раньше жила в generic Domain (HA_ENTITY_ZB_DIAG_DEVICE/ENDPOINT);
 * после Шага C наружу не выходит.
 *
 * Топология переоткрывается интервью на каждом старте, поэтому хранилище — RAM: identity
 * детерминирована, persistence не требуется (см. zigbee_entity_registry).
 */

#include <stdbool.h>
#include <stdint.h>

#include "zigbee/ha_zigbee.h"    /* ha_device_uid_t */
#include "zigbee/zigbee_forms.h" /* ha_device_record_t, ha_endpoint_record_t, ha_cluster_entry_t */
#include "sys/sys_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Топология одного endpoint'а из Simple Descriptor (интервью). */
typedef struct {
    ha_device_uid_t device_uid;
    uint8_t endpoint;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t cluster_count;
    const ha_cluster_entry_t *clusters;
} zigbee_endpoint_desc_t;

/* Записать модель из Basic-кластера, сохранив имя. out_changed — модель изменилась. */
/* Записать идентичность из Basic-кластера (manufacturer 0x0004 + model 0x0005). */
sys_error_t zigbee_device_set_identity(ha_device_uid_t uid, const char *manufacturer,
                                       const char *model, bool *out_changed);

/* Прочитать запись устройства. false — устройства нет. */
bool zigbee_device_get(ha_device_uid_t uid, ha_device_record_t *out);

/* Устройство известно адаптеру (после интервью)? */
bool zigbee_device_has(ha_device_uid_t uid);

/* Записать endpoint. out_changed — топология изменилась. Кластеров больше лимита — INVALID_ARG. */
sys_error_t zigbee_topology_apply(const zigbee_endpoint_desc_t *desc, bool *out_changed);

/* Прочитать endpoint. false — записи нет. */
bool zigbee_topology_get(ha_device_uid_t uid, uint8_t endpoint, ha_endpoint_record_t *out);

/*
 * Устройство ушло из сети: снять его запись и все endpoint'ы. Идемпотентно: чего нет,
 * то не мешает.
 */
sys_error_t zigbee_device_remove(ha_device_uid_t uid);

/* --- persistence (flash) ------------------------------------------------- */

/*
 * Топология/модель устройств переживает reboot: сохраняем в раздел flash "zb_model"
 * (блоб + magic/crc). После загрузки адаптер восстанавливает устройства без интервью.
 */
sys_error_t zigbee_device_store_load(void);
sys_error_t zigbee_device_store_save(void);

typedef bool (*zigbee_device_visit_fn)(ha_device_uid_t uid, const ha_device_record_t *record,
                                       void *ctx);
void zigbee_device_store_foreach(zigbee_device_visit_fn fn, void *ctx);

typedef bool (*zigbee_endpoint_visit_fn)(uint8_t endpoint, const ha_endpoint_record_t *record,
                                         void *ctx);
void zigbee_endpoints_foreach(ha_device_uid_t uid, zigbee_endpoint_visit_fn fn, void *ctx);

uint8_t zigbee_endpoint_count(ha_device_uid_t uid);

#ifdef __cplusplus
}
#endif
