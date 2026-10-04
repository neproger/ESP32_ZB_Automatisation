#pragma once

/*
 * Формы команд (docs/RECORD_MODEL.md §9, docs/domain/COMMANDS.md §7).
 *
 * Два разных уровня адресации не смешиваются:
 *
 *   Domain Command      — executor/type + opaque payload. Domain не знает, что внутри.
 *   Zigbee command      — device_uid + dst_endpoint + cluster/command + аргументы.
 *
 * Чего здесь нет и не будет:
 *   - src_endpoint: внутренняя политика Zigbee service/gateway;
 *   - short address: текущее сетевое состояние, а не identity — сервис разрешает
 *     device_uid → network address сам;
 *   - ezb_* структур esp-zigbee-sdk: они живут только внутри Zigbee service.
 */

#include <stdint.h>

#include "ha_model/ha_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Дискриминатор команды Domain: по нему выбирается executor. Это общий идентификатор
 * для Automation (кто постит) и Zigbee service (кто исполняет), а не предметная
 * семантика.
 */
#define HA_CMD_ZIGBEE_CLUSTER 1u

/* Пометить устройство на удаление (args = ha_device_uid_t): leave при появлении. */
#define HA_CMD_DEVICE_REMOVE 2u

/* Предел аргументов ZCL-команды: форма фиксирована, как и любая форма словаря. */
#define HA_ZB_COMMAND_ARGS_MAX 16u

/*
 * Адресат описывается устройством и endpoint: кластер и команда берутся из
 * ha_zigbee.h, аргументы — уже в кодировке ZCL (без масштабирования: 0..254 уровень,
 * 0.01 °C и т. п. разбирает сервис).
 */
typedef struct {
    ha_device_uid_t device_uid;
    uint8_t dst_endpoint;
    uint16_t cluster_id;
    uint8_t command_id;
    uint8_t args_len;
    uint8_t args[HA_ZB_COMMAND_ARGS_MAX];
} ha_zb_command_t;

#ifdef __cplusplus
}
#endif
