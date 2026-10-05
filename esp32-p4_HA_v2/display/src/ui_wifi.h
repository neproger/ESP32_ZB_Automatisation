#pragma once

/*
 * Экран Wi-Fi: список найденных точек (HA_ENTITY_WIFI_SCAN), статус подключения
 * (HA_ENTITY_WIFI_STATUS), кнопка «Сканировать» и диалог пароля с клавиатурой.
 * Экраном/переходами владеет display.c; здесь — построение и обновление.
 */

#include "lvgl.h"

#include "domain/domain.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ui_wifi_back_cb)(void);

/* Строит экран и возвращает его root. Владелец (display.c) грузит и удаляет его сам. */
lv_obj_t *ui_wifi_create(domain_t *domain, ui_wifi_back_cb back);

/* Обновление из Domain: статус подключения и список точек. */
void ui_wifi_apply(void);

#ifdef __cplusplus
}
#endif
