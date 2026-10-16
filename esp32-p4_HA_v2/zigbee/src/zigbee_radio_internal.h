#pragma once

#include <stdbool.h>

#include "ezbee/app_signals.h"
#include "ezbee/zcl/zcl_core.h"
#include "ha_model/ha_entities.h"
#include "zigbee/ha_zigbee.h"

/*
 * Внутренняя связь трёх файлов радиосервиса:
 *   zigbee_radio.c            — подъём стека/RCP, задача, отправка;
 *   zigbee_radio_reports.c    — декод репортов, интeрвью, report/event-обработчики;
 *   zigbee_radio_commissioning.c — BDB-сигналы, commissioning, leave.
 * Наружу (zigbee.h) ничего из этого не выходит.
 */

/* Endpoint координатора: несут client-кластеры и ZCL-кадры. */
#define COORDINATOR_ENDPOINT 1

/* Стек готов принимать команды. Пишет задача радио при старте; читают сеть/leave. */
extern volatile bool zigbee_radio_ready;

/* Обработчики стека, регистрируются при подъёме (zigbee_radio.c). */
void core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message);
bool raw_frame_handler(const ezb_zcl_raw_frame_t *raw);
bool app_signal_handler(const ezb_app_signal_t *signal);

/* Интервью (zigbee_radio_reports.c): запускает/снимает commissioning. */
void interview_begin(ha_device_uid_t uid, uint16_t short_addr);
void interview_drop(ha_device_uid_t uid);
