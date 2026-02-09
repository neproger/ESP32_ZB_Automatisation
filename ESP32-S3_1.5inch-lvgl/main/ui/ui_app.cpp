#include "ui_app.hpp"

#include <algorithm>
#include <cstdint>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "iot_button.h"
#include "iot_knob.h"
#include "devices_init.h"

namespace
{
    static const char *TAG_UI = "ui_min";

    lv_obj_t *s_status = nullptr;
    lv_obj_t *s_knob_value = nullptr;
    lv_obj_t *s_touch_value = nullptr;
    lv_obj_t *s_button_value = nullptr;

    int s_knob_pos = 0;
    int s_touch_count = 0;
    int s_button_count = 0;

    void update_labels()
    {
        if (s_knob_value)
        {
            lv_label_set_text_fmt(s_knob_value, "Encoder value: %d", s_knob_pos);
        }
        if (s_touch_value)
        {
            lv_label_set_text_fmt(s_touch_value, "Touch taps: %d", s_touch_count);
        }
        if (s_button_value)
        {
            lv_label_set_text_fmt(s_button_value, "Button clicks: %d", s_button_count);
        }
    }

    void touch_cb(lv_event_t *e)
    {
        if (!e)
        {
            return;
        }

        lv_event_code_t code = lv_event_get_code(e);
        lv_indev_t *indev = lv_indev_active();
        lv_point_t p = {0, 0};
        if (indev)
        {
            lv_indev_get_point(indev, &p);
        }

        if (code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED)
        {
            if (s_status)
            {
                lv_label_set_text_fmt(s_status, "Touch: x=%d y=%d", p.x, p.y);
            }
        }
        else if (code == LV_EVENT_CLICKED)
        {
            s_touch_count++;
            if (s_status)
            {
                lv_label_set_text_fmt(s_status, "Touch click: x=%d y=%d", p.x, p.y);
            }
            update_labels();
        }
    }
} // namespace

void ui_app_init(void)
{
    lvgl_port_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Hardware base ready");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, "Touch/rotate/click to test input");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x93c5fd), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_set_size(panel, 360, 220);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 25);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x334155), 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_knob_value = lv_label_create(panel);
    s_touch_value = lv_label_create(panel);
    s_button_value = lv_label_create(panel);

    lv_obj_set_style_text_color(s_knob_value, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_color(s_touch_value, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_color(s_button_value, lv_color_hex(0xf8fafc), 0);

    update_labels();

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, "Button click only logs/counter");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x94a3b8), 0);

    (void)devices_display_set_brightness(230);
    if (!devices_has_touch())
    {
        lv_label_set_text(s_status, "Touch driver not initialized");
        ESP_LOGW(TAG_UI, "Touch handle is not available");
    }

    lvgl_port_unlock();
}

extern "C" void LVGL_knob_event(void *event)
{
    int ev = (int)(intptr_t)event;

    if (ev == KNOB_RIGHT)
    {
        s_knob_pos = std::min(s_knob_pos + 1, 999);
    }
    else if (ev == KNOB_LEFT)
    {
        s_knob_pos = std::max(s_knob_pos - 1, -999);
    }

    update_labels();
}

extern "C" void LVGL_button_event(void *event)
{
    int ev = (int)(intptr_t)event;

    if (ev == BUTTON_SINGLE_CLICK)
    {
        s_button_count++;
        if (s_status)
        {
            lv_label_set_text(s_status, "Button single click");
        }
        ESP_LOGI(TAG_UI, "Button single click");
    }

    update_labels();
}
