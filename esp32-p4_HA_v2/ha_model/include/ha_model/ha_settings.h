#pragma once

/*
 * Настройки приложения (docs/services/SETTINGS.md): одна запись в Domain. Поля
 * добавляются вместе с реальным потребителем — не заранее. Сейчас это подсветка,
 * таймаут скринсейвера и тема UI; погодные интервалы и прочее появятся, когда будет сервис.
 *
 * Регистрируется из bootstrap (docs/RECORD_MODEL.md §1.1), читает/пишет — Web и Display.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Единственная запись настроек. */
#define HA_SETTINGS_ID 0

typedef struct {
    uint8_t id;
} ha_settings_key_t;

typedef struct {
    uint32_t screensaver_timeout_ms; /* 0 — скринсейвер выключен */
    uint8_t brightness_pct;          /* подсветка, 0..100 */
    uint8_t palette_id;              /* тема UI (ui_palette_id_t); 0 — по умолчанию */
    uint8_t reserved[2];             /* явный padding, RECORD_MODEL §2 */
} ha_settings_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_settings_key_t) == 1, "ha_settings_key_t: неожиданный размер");
static_assert(sizeof(ha_settings_record_t) == 8, "ha_settings_record_t: неожиданный размер");
#else
_Static_assert(sizeof(ha_settings_key_t) == 1, "ha_settings_key_t: неожиданный размер");
_Static_assert(sizeof(ha_settings_record_t) == 8, "ha_settings_record_t: неожиданный размер");
#endif

#ifdef __cplusplus
}
#endif
