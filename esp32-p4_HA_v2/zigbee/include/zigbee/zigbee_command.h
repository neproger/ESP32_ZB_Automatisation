#pragma once

#include "domain/domain.h"
#include "ha_model/ha_commands.h"
#include "sys/sys_error.h"

/*
 * Проверка команды до отправки (docs/domain/COMMANDS.md §5, docs/services/ZIGBEE.md §5).
 *
 * Здесь только то, что сервис может проверить по своим данным: адресат должен быть
 * известен системе. Разбор ZCL-аргументов, масштабирование и структуры esp-zigbee-sdk
 * появляются внутри сервиса и на транспорте, а не в этой проверке.
 *
 * FreeRTOS и логирования нет, поэтому проверка собирается и тестируется на хосте.
 */
sys_error_t zigbee_command_check(domain_t *domain, const ha_zb_command_t *command);
