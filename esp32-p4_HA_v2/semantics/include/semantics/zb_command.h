#pragma once

/*
 * Физическая ZCL-команда (Шаг C, C2): transport/ZCL-private форма. Живёт в мосте
 * `semantics` (владелец ZCL-кодирования), НЕ в transport-agnostic `ha_model`.
 * Zigbee adapter получает её из `semantics_build_command` и отправляет.
 */

#include <stdint.h>

#include "ha_model/ha_zigbee.h" /* ha_device_uid_t, HA_ZB_CLUSTER_*, HA_ZB_CMD_* */

#ifdef __cplusplus
extern "C" {
#endif

/* Предел аргументов ZCL-команды: форма фиксирована. */
#define HA_ZB_COMMAND_ARGS_MAX 16u

/*
 * Адресат описывается устройством и endpoint; кластер/команда — из ha_zigbee.h; аргументы —
 * уже в кодировке ZCL (масштабирование разбирает сервис).
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
static_assert(sizeof(ha_zb_command_t) == 32, "ha_zb_command_t: неожиданный размер");
#endif

#ifdef __cplusplus
}
#endif
