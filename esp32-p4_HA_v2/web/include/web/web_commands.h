#pragma once

#include <stddef.h>
#include <stdint.h>

#include "domain/domain.h"

/*
 * Browser → Device: args каждой команды — canonical struct verbatim; модуль декодирует
 * их в Domain-операции (post/put/remove). Отклонённые команды логируются, но не рвут
 * сессию.
 */

void web_commands_init(domain_t *domain);

/* Обработать команду (cmd + args, как пришли на проводе). */
void web_commands_handle(uint8_t cmd, const uint8_t *args, size_t len);
