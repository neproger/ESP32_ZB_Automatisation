/*
 * Точка входа host-превью (LVGL Live Preview): расширение само создаёт display/indev,
 * вызывает lvgl_live_preview_init() и крутит lv_timer_handler. UI — тот же, что в
 * прошивке; вместо Domain подставлен фейк с мок-данными (domain_fake.c).
 */

#include "lvgl.h"

#include "display/display.h"

/* Реализация в domain_fake.c; на прошивке её нет — там настоящий Domain. */
void domain_fake_init(void);

/* Расширение LVGL Live Preview само определяет LVGL_LIVE_PREVIEW и зовёт эту функцию. */
#ifdef LVGL_LIVE_PREVIEW
void lvgl_live_preview_init(void)
{
    domain_fake_init();

    domain_t domain = {0}; /* фейк игнорирует handle, данные — статические мок-фикстуры */
    display_start(&domain);
}
#endif
