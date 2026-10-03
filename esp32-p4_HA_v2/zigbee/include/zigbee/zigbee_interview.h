#pragma once

#include "domain/domain.h"
#include "ha_model/ha_entities.h"
#include "sys/sys_error.h"

/*
 * Интервью устройства (docs/services/ZIGBEE.md §3.1, §9.4). Радио собирает Simple
 * Descriptor и Basic в эту форму; Domain она попадает только после завершения
 * интервью — устройство без интервью живёт лишь в RAM слоя Zigbee.
 *
 * Здесь нет FreeRTOS и нет SDK: формулировка топологии из ответов радио (роли
 * кластеров) и запись устройства — чистая логика, тестируемая на хосте.
 */

#define ZIGBEE_INTERVIEW_ENDPOINTS_MAX 8

typedef struct {
    uint8_t endpoint;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t cluster_count;
    ha_cluster_entry_t clusters[HA_ENDPOINT_CLUSTERS_MAX];
} zigbee_interview_endpoint_t;

typedef struct {
    ha_device_uid_t uid;
    char model[HA_DEVICE_MODEL_MAX];
    uint8_t endpoint_count;
    zigbee_interview_endpoint_t endpoints[ZIGBEE_INTERVIEW_ENDPOINTS_MAX];
} zigbee_interview_result_t;

/*
 * Разложить кластеры Simple Descriptor по ролям: input = server, output = client
 * (docs/services/ZIGBEE_CAPABILITIES.md §1). Больше out_max — INVALID_ARG: молча
 * обрезать состав значило бы потерять часть возможностей устройства.
 */
sys_error_t zigbee_clusters_from_lists(const uint16_t *input, uint8_t input_count,
                                       const uint16_t *output, uint8_t output_count,
                                       ha_cluster_entry_t *out, uint8_t out_max,
                                       uint8_t *out_count);

/* Записать интервью в Domain: устройство (модель из Basic) и топология endpoint'ов. */
sys_error_t zigbee_interview_apply(domain_t *domain, const zigbee_interview_result_t *result);
