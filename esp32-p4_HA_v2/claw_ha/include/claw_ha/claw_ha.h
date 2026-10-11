#pragma once

/*
 * ESP-Claw как локальный потребитель Domain (как Automation/Display). Framework-
 * компоненты esp-claw (claw_core/claw_cap) подключены без их shell (app_claw/
 * app_config/board/FATFS). Домен о Claw не знает; JSON/Claw-типы не выходят наружу
 * этого компонента.
 */

#include "domain/domain.h"
#include "sys/sys_error.h"

/* Клиентский слой (объявлен здесь; generic sys знает только core-слои). */
#define SYS_LAYER_CLAW_HA ((sys_layer_t)9)

/* Поднять Claw-агента поверх готового Domain. Вызывается из bootstrap. */
sys_error_t claw_ha_start(domain_t *domain);
