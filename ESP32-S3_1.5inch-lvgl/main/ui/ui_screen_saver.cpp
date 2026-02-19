#include "ui_screen_saver.hpp"

#include <cstdint>
#include <time.h>

#include "gw_core/net_time.h"
#include "ui_style.hpp"

namespace
{
lv_obj_t *s_overlay = nullptr;
lv_obj_t *s_stack = nullptr;
lv_obj_t *s_time = nullptr;
ui_screen_saver_wakeup_cb_t s_wakeup_cb = nullptr;
uint64_t s_last_rendered_second = UINT64_MAX;

void on_overlay_input(lv_event_t *event)
{
    (void)event;
    if (s_wakeup_cb) {
        s_wakeup_cb();
    }
}
} // namespace

void ui_screen_saver_init(lv_obj_t *root, ui_screen_saver_wakeup_cb_t wakeup_cb)
{
    if (!root) {
        return;
    }

    s_wakeup_cb = wakeup_cb;
    s_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    s_stack = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_stack);
    lv_obj_set_size(s_stack, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_stack, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_stack, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_stack, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_stack, 10, 0);

    s_time = lv_label_create(s_stack);
    lv_label_set_text(s_time, "--:--:--");
    lv_obj_set_style_text_color(s_time, lv_color_hex(ui_style::kTitleTextHex), 0);
    lv_obj_set_style_text_font(s_time, ui_style::kFontTitle, 0);

    lv_obj_t *hint = lv_label_create(s_stack);
    lv_label_set_text(hint, "Touch / knob / button");
    lv_obj_set_style_text_color(hint, lv_color_hex(ui_style::kSubtitleTextHex), 0);
    lv_obj_set_style_text_font(hint, ui_style::kFontSubtitle, 0);

    lv_obj_add_event_cb(s_overlay, on_overlay_input, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_overlay, on_overlay_input, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_overlay, on_overlay_input, LV_EVENT_GESTURE, nullptr);
}

void ui_screen_saver_show(bool show)
{
    if (!s_overlay) {
        return;
    }

    if (show) {
        lv_obj_move_foreground(s_overlay);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

bool ui_screen_saver_is_visible(void)
{
    return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_screen_saver_tick(void)
{
    if (!s_time || !ui_screen_saver_is_visible()) {
        return;
    }

    const uint64_t now_ms = gw_net_time_now_ms();
    if (now_ms == 0) {
        if (s_last_rendered_second != 0) {
            lv_label_set_text(s_time, "--:--:--");
            s_last_rendered_second = 0;
        }
        return;
    }

    const uint64_t second = now_ms / 1000ULL;
    if (second == s_last_rendered_second) {
        return;
    }

    time_t ts = (time_t)(second);
    struct tm tm_now = {};
    localtime_r(&ts, &tm_now);

    char buf[16] = {};
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_now);
    lv_label_set_text(s_time, buf);
    s_last_rendered_second = second;
}
