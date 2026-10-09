#pragma once

#include "ezbee/af.h"
#include "ha_model/ha_zigbee.h"
#include "zigbee/zigbee_interview.h"

/*
 * Подписка координатора на состояние устройства (docs/services/ZIGBEE.md §4).
 *
 * После интервью устройство не шлёт репорты само: координатор создаёт binding
 * server-кластеров устройства на себя и настраивает reporting. Чтобы репорт принялся
 * и команда ушла, у координатора должен быть client-кластер того же типа — его
 * объявляет zigbee_binding_add_client_clusters() при создании устройства.
 *
 * Модуль только IDF: говорит со стеком, результат кладёт не в Domain.
 */

/* client-кластеры координатора для кластеров, по которым настроен reporting. */
void zigbee_binding_add_client_clusters(ezb_af_ep_desc_t coordinator_endpoint);

/* binding server-кластеров устройства на координатор + Configure Reporting. */
void zigbee_binding_apply(ha_device_uid_t uid, uint16_t short_addr,
                          const zigbee_interview_endpoint_t *endpoints, uint8_t endpoint_count);

/* Список reportable-атрибутов кластера (для Read Attributes после интервью). */
uint8_t zigbee_binding_attrs_for(uint16_t cluster_id, uint16_t *out, uint8_t max);
