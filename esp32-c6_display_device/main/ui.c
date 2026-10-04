#include "ui.h"
#include "display.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define UI_LOG_LINES 5

static bool s_ready;
static lv_obj_t *s_status;
static lv_obj_t *s_info;
static lv_obj_t *s_temp;
static lv_obj_t *s_light;
static lv_obj_t *s_button;
static lv_obj_t *s_log;

static char s_log_lines[UI_LOG_LINES][48];
static int s_log_count;

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    return lbl;
}

void ui_init(void)
{
    if (!display_lock(1000)) {
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 4, 0);

    make_label(scr, &lv_font_montserrat_14, 0x8AB4F8); /* title */

    s_status = make_label(scr, &lv_font_montserrat_20, 0xFFB300);
    lv_label_set_text(s_status, "BOOTING");

    s_info = make_label(scr, &lv_font_montserrat_14, 0xFFFFFF);
    lv_label_set_text(s_info, "PAN: ----\nCH: --\nADDR: ----\nIEEE: --");

    s_temp = make_label(scr, &lv_font_montserrat_14, 0xFF8A65);
    lv_label_set_text(s_temp, "TEMP: --.- C");

    s_light = make_label(scr, &lv_font_montserrat_14, 0xFFFFFF);
    lv_label_set_text(s_light, "LIGHT: OFF\nLEVEL: 0\nHUE: 0 SAT: 0");

    s_button = make_label(scr, &lv_font_montserrat_14, 0xFFFFFF);
    lv_label_set_text(s_button, "BTN: 0");

    s_log = make_label(scr, &lv_font_montserrat_14, 0xE0F7FA);
    lv_label_set_text(s_log, "log...");

    s_ready = true;
    display_unlock();
}

void ui_set_network_state(const char *state, uint32_t rgb)
{
    if (!s_ready || !display_lock(1000)) {
        return;
    }
    lv_label_set_text(s_status, state);
    lv_obj_set_style_text_color(s_status, lv_color_hex(rgb), 0);
    display_unlock();
}

void ui_set_net_info(uint16_t pan_id, uint8_t channel, uint16_t short_addr, const char *ieee_str)
{
    if (!s_ready || !display_lock(1000)) {
        return;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "PAN: 0x%04X\nCH: %u\nADDR: 0x%04X\nIEEE: %s", pan_id, channel, short_addr,
             ieee_str ? ieee_str : "--");
    lv_label_set_text(s_info, buf);
    display_unlock();
}

void ui_set_light(bool on, uint8_t level, uint8_t hue, uint8_t sat)
{
    if (!s_ready || !display_lock(1000)) {
        return;
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "LIGHT: %s\nLEVEL: %u\nHUE: %u SAT: %u", on ? "ON" : "OFF", level, hue, sat);
    lv_label_set_text(s_light, buf);
    display_unlock();
}

void ui_set_temperature(float celsius)
{
    if (!s_ready || !display_lock(1000)) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "TEMP: %.1f C", celsius);
    lv_label_set_text(s_temp, buf);
    display_unlock();
}

void ui_set_button_count(uint32_t count)
{
    if (!s_ready || !display_lock(1000)) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "BTN: %u", (unsigned)count);
    lv_label_set_text(s_button, buf);
    display_unlock();
}

void ui_add_log(const char *fmt, ...)
{
    if (!s_ready) {
        return;
    }

    char line[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (!display_lock(1000)) {
        return;
    }

    if (s_log_count < UI_LOG_LINES) {
        strncpy(s_log_lines[s_log_count], line, sizeof(s_log_lines[0]) - 1);
        s_log_lines[s_log_count][sizeof(s_log_lines[0]) - 1] = '\0';
        s_log_count++;
    } else {
        memmove(s_log_lines[0], s_log_lines[1], sizeof(s_log_lines[0]) * (UI_LOG_LINES - 1));
        strncpy(s_log_lines[UI_LOG_LINES - 1], line, sizeof(s_log_lines[0]) - 1);
        s_log_lines[UI_LOG_LINES - 1][sizeof(s_log_lines[0]) - 1] = '\0';
    }

    char text[UI_LOG_LINES * 48];
    text[0] = '\0';
    for (int i = 0; i < s_log_count; i++) {
        if (i) {
            strncat(text, "\n", sizeof(text) - strlen(text) - 1);
        }
        strncat(text, s_log_lines[i], sizeof(text) - strlen(text) - 1);
    }
    lv_label_set_text(s_log, text);

    display_unlock();
}
