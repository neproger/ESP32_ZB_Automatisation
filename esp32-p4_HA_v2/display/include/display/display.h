#pragma once

/*
 * Display — клиент Domain (docs/clients/DISPLAY.md). Строит LVGL-экраны из данных
 * Domain (group / group_item / endpoint / state) и периодически опрашивает их через
 * domain_entity_meta, чтобы перерисовывать только изменившееся.
 *
 * ui/ зависит от LVGL + Domain API + ha_model и не знает про esp_lcd/lvgl_port:
 * дисплей и тач ему отдаёт платформенный port (порт/превью).
 */

#include <stdbool.h>

#include "domain/domain.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Создаёт экраны и запускает цикл отрисовки. Вызывается после domain_init. */
void display_start(domain_t *domain);

/* Шаг опроса: meta -> get -> render_if_needed. Зовётся из задачи LVGL (или превью). */
void display_poll(void);

#ifdef __cplusplus
}
#endif
