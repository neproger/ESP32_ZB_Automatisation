#pragma once

/*
 * Display — клиент Domain (docs/clients/DISPLAY.md). Строит LVGL-экраны из данных
 * Domain (group / group_item / entity / state) и обновляется **по подписке** на факты
 * Domain (как web): try_push копирует факт в inbox, обработка — в задаче LVGL.
 *
 * ui/ зависит от LVGL + Domain API + ha_model и не знает про esp_lcd/lvgl_port:
 * дисплей и тач ему отдаёт платформенный port (порт/превью).
 */

#include <stdbool.h>
#include <stdint.h>

#include "domain/domain.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Создаёт экраны и запускает цикл отрисовки. Вызывается после domain_init. */
void display_start(domain_t *domain);

/* Дренаж inbox подписки: применить накопленные факты Domain. Зовётся из задачи LVGL. */
void display_process_events(void);

/*
 * Пересобрать UI под палитру из settings (экран настроек меняет тему). Отложенно, в
 * следующем кадре, чтобы не удалять экран внутри его обработчика события.
 */
void display_request_theme_reload(void);

/*
 * Подсветка панели. Привод (LEDC) регистрирует порт (display_p4); UI/настройки только
 * задают процент. Привод не зарегистрирован — no-op (host-превью).
 */
typedef void (*display_brightness_fn)(uint8_t percent);
void display_register_brightness(display_brightness_fn fn);
void display_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif
