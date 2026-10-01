#pragma once

#include "domain/domain.h"
#include "ha_model/ha_entities.h"
#include "sys/sys_error.h"

/*
 * Топология устройства: состав endpoint'ов и кластеров с ролями
 * (docs/services/ZIGBEE.md §3). Живёт в Domain отдельной сущностью, потому что её
 * читают не только Zigbee service: Web показывает возможности, Automation по ним
 * строит правила.
 *
 * Состояние атрибута здесь не лежит никогда: оно адресуется отдельно, на атрибут.
 */

typedef struct {
    ha_device_uid_t device_uid;
    uint8_t endpoint;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t cluster_count;
    const ha_cluster_entry_t *clusters;
} zigbee_endpoint_desc_t;

/*
 * Записать endpoint. out_changed — «топология изменилась»: повтор того же состава
 * не даёт ни факта, ни изменения. Кластеров больше лимита — INVALID_ARG.
 */
sys_error_t zigbee_topology_apply(domain_t *domain, const zigbee_endpoint_desc_t *desc,
                                  bool *out_changed);
