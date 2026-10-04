#pragma once

#include "domain/domain.h"
#include "ha_model/ha_commands.h"
#include "sys/sys_error.h"

/*
 * Радиоканал: ESP-Hosted по SDIO поднимает RCP на встроенном C6, поверх него — стек
 * Zigbee на P4 (docs/services/ZIGBEE.md §5, плата GUITION JC4880P443C).
 *
 * Принятые репорты не пишутся в Domain отсюда: они складываются в очередь сервиса
 * (zigbee_submit_report), а запись идёт в задаче сервиса.
 */
sys_error_t zigbee_radio_start(domain_t *domain);

/*
 * Отправить ZCL-команду в сеть. Вызывающий — задача сервиса; стек сам сериализует
 * запросы из чужого контекста. INVALID_STATE — стек ещё не стартовал (команда не ушла),
 * INVALID_ARG — кластер/команда/аргументы вне формы, IO — радио отказало.
 */
sys_error_t zigbee_radio_send(const ha_zb_command_t *command);

/*
 * Отправить устройству ZDO Mgmt_Leave (удаление из сети). NOT_FOUND — устройства нет
 * в сети прямо сейчас (leave уйдёт при следующем announce); INVALID_STATE — стек не
 * стартовал; IO — радио отказало.
 */
sys_error_t zigbee_radio_request_leave(ha_device_uid_t uid);

/*
 * Открыть сеть для подключения новых устройств на seconds секунд (0 — закрыть).
 * Вызывается только по команде из UI; на старте сеть закрыта.
 */
sys_error_t zigbee_radio_open_network(uint8_t seconds);
