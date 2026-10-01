#pragma once

#include "domain/domain.h"

/*
 * Журнал в консоль: подписчик печатает каждую запись, доставленную Dispatcher'ом.
 * Журнал читает только Dispatcher, прямого read-пути нет (docs/domain/JOURNAL.md §5).
 *
 * Печать идёт в контексте Dispatcher'а под dispatch_lock, поэтому это наблюдение за
 * системой, а не часть боевого пути: при появлении сервисов со своими inbox логирование
 * уйдёт в задачу подписчика.
 */
sys_error_t journal_console_subscribe(domain_t *domain);
