#pragma once

#include "domain/domain.h"
#include "sys/sys_error.h"

/*
 * Automation service (docs/services/AUTOMATION.md): подписчик фактов Event от Zigbee.
 * При срабатывании правила постит Zigbee-команду через Domain, а не дёргает радио.
 *
 * Поднимается из bootstrap после domain_init() и регистрации типов.
 */
sys_error_t automation_start(domain_t *domain);
