#pragma once

#include "domain/domain.h"

/*
 * Журнал в консоль: подписчик печатает каждую запись, доставленную Dispatcher'ом.
 * Журнал читает только Dispatcher, прямого read-пути нет (docs/domain/JOURNAL.md §5).
 *
 * Подписчик имеет свой inbox и задачу (docs/domain/DISPATCHER.md §1): Dispatcher только
 * кладёт факт в очередь, печать в UART идёт в задаче подписчика и не тормозит доставку
 * остальным. Очередь конечна: при перегрузке диагностика теряется, факты — нет.
 */
sys_error_t journal_console_subscribe(domain_t *domain);
