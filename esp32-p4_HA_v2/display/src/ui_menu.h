#pragma once

/*
 * Выпадающее меню «бургера» в строке состояния: живёт на lv_layer_top, поверх экрана.
 * Пункты — переходы, решения принимает display.c через колбэк.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_MENU_SETTINGS = 0,
    UI_MENU_WIFI,
} ui_menu_item_t;

typedef void (*ui_menu_cb)(ui_menu_item_t item);

void ui_menu_create(void);
void ui_menu_set_cb(ui_menu_cb cb);
void ui_menu_toggle(void);
void ui_menu_hide(void);

#ifdef __cplusplus
}
#endif
