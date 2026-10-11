#pragma once

/*
 * Deferred-leave (Шаг C, C6): Zigbee-private пометка «устройство просили удалить».
 * Устройство нельзя убрать из сети напрямую — держим uid, пока оно не появится в сети,
 * и шлём ZDO Mgmt_Leave. Пометка переживает reboot (NVS) и наружу не выходит: generic
 * Domain о ней не знает.
 */

#include <stdbool.h>

#include "zigbee/ha_zigbee.h" /* ha_device_uid_t */
#include "sys/sys_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Пометить uid (идемпотентно). NO_MEM — если таблица полна. */
sys_error_t zigbee_remove_mark_add(ha_device_uid_t uid);

/* Пометка стоит? */
bool zigbee_remove_mark_has(ha_device_uid_t uid);

/* Снять пометку (после успешного leave). Нет пометки — не ошибка. */
void zigbee_remove_mark_clear(ha_device_uid_t uid);

#ifdef __cplusplus
}
#endif
