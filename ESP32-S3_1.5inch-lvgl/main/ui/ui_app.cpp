#include "ui_app.hpp"

#include <cstdint>
#include <cstring>
#include <atomic>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "iot_knob.h"
#include "freertos/task.h"

#include "devices_init.h"
#include "ui_control_ack.hpp"
#include "ui_screen_devices.hpp"
#include "ui_screen_saver.hpp"
#include "ui_style.hpp"
#include "gw_model/gw_model_settings.h"

namespace
{
static const char *TAG_UI = "ui_app";

static bool s_display_enabled = true;
static bool s_ui_ready = false;
static bool s_saver_active = false;
static lv_obj_t *s_splash = nullptr;
static std::atomic<bool> s_pending_button_click{false};
static std::atomic<bool> s_pending_user_activity{false};
static constexpr uint8_t kDisplayBrightness80Pct = 204;
static constexpr uint32_t kUiTickPeriodMs = 16;
static constexpr uint32_t kControlAckTimeoutMs = 1800;

void log_ui_boot_stack_hwm(const char *stage)
{
    ESP_LOGI(TAG_UI, "ui_boot stack_hwm after %s: %u", stage ? stage : "?", (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

uint32_t screensaver_timeout_ms()
{
    static uint32_t cached_timeout_ms = 10000;
    static uint64_t last_read_ms = 0;
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (last_read_ms != 0 && (now_ms - last_read_ms) < 1000) {
        return cached_timeout_ms;
    }

    gw_proto_settings_v1_t cfg = {};
    if (gw_model_get_settings(&cfg) == ESP_OK) {
        cached_timeout_ms = cfg.screensaver_timeout_ms;
        last_read_ms = now_ms;
        return cached_timeout_ms;
    }
    last_read_ms = now_ms;
    return cached_timeout_ms;
}

void splash_show(bool show)
{
    if (!s_splash) {
        return;
    }
    if (show) {
        lv_obj_move_foreground(s_splash);
        lv_obj_clear_flag(s_splash, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_splash, LV_OBJ_FLAG_HIDDEN);
    }
}

void splash_init(lv_obj_t *root)
{
    s_splash = lv_obj_create(root);
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_splash, lv_color_hex(ui_style::kScreenBgHex), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *stack = lv_obj_create(s_splash);
    lv_obj_remove_style_all(stack);
    lv_obj_set_size(stack, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(stack, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(stack, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stack, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(stack, 10, 0);

    lv_obj_t *title = lv_label_create(stack);
    lv_label_set_text(title, "Загрузка...");
    lv_obj_set_style_text_color(title, lv_color_hex(ui_style::kTitleTextHex), 0);
    lv_obj_set_style_text_font(title, ui_style::kFontTitle, 0);

    lv_obj_t *hint = lv_label_create(stack);
    lv_label_set_text(hint, "Синхронизация данных");
    lv_obj_set_style_text_color(hint, lv_color_hex(ui_style::kSubtitleTextHex), 0);
    lv_obj_set_style_text_font(hint, ui_style::kFontSubtitle, 0);
}

void wake_from_screensaver()
{
    if (!s_ui_ready || !s_saver_active) {
        return;
    }
    s_saver_active = false;
    ui_screen_saver_show(false);
}

void note_user_activity()
{
    lv_display_t *display = lv_display_get_default();
    if (display) {
        lv_display_trigger_activity(display);
    }
}

void ui_gesture_cb(lv_event_t *event)
{
    (void)event;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }

    note_user_activity();
    if (!s_ui_ready) {
        return;
    }
    if (s_saver_active) {
        wake_from_screensaver();
        return;
    }

    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_BOTTOM) {
        ui_screen_devices_step_item(-1);
    } else if (dir == LV_DIR_TOP) {
        ui_screen_devices_step_item(1);
    } else if (dir == LV_DIR_RIGHT) {
        ui_screen_devices_step_group(-1);
    } else if (dir == LV_DIR_LEFT) {
        ui_screen_devices_step_group(1);
    }
}

void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    const bool user_activity = s_pending_user_activity.exchange(false, std::memory_order_acq_rel);
    const bool button_click = s_pending_button_click.exchange(false, std::memory_order_acq_rel);

    if (user_activity) {
        note_user_activity();
        if (s_ui_ready && s_saver_active) {
            wake_from_screensaver();
            ui_screen_devices_clear_pending_navigation();
        }
    }

    if (button_click) {
        note_user_activity();
        if (s_ui_ready && !s_saver_active) {
            s_display_enabled = !s_display_enabled;
            if (s_display_enabled)
            {
                (void)devices_display_set_enabled(true);
                (void)devices_display_set_brightness(kDisplayBrightness80Pct);
            }
            else
            {
                (void)devices_display_set_enabled(false);
            }
        } else if (s_saver_active) {
            wake_from_screensaver();
        }
    }

    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    ui_control_ack_poll_timeouts(now_ms, kControlAckTimeoutMs);

    if (!s_ui_ready) {
        s_ui_ready = true;
        splash_show(false);
        s_saver_active = false;
        note_user_activity();
    }

    if (s_ui_ready && !s_saver_active)
    {
        ui_screen_devices_render_if_needed();
    }

    lv_display_t *display = lv_display_get_default();
    const uint32_t inactive_ms = display ? lv_display_get_inactive_time(display) : 0;
    if (s_ui_ready && !s_saver_active && inactive_ms >= screensaver_timeout_ms())
    {
        s_saver_active = true;
        ui_screen_saver_show(true);
    }
    ui_screen_saver_tick();

}
} // namespace

void ui_app_init(void)
{
    log_ui_boot_stack_hwm("ui_app.begin");
    lvgl_port_lock(-1);

    ui_screen_devices_init(lv_screen_active());
    ui_screen_saver_init(lv_layer_top(), wake_from_screensaver);
    splash_init(lv_layer_top());
    splash_show(true);
    s_ui_ready = false;
    lv_timer_create(ui_tick_cb, kUiTickPeriodMs, nullptr);

    lvgl_port_unlock();
    log_ui_boot_stack_hwm("ui_app.ready");
    ESP_LOGI(TAG_UI, "Group UI initialized");
}

void ui_app_attach_screen_input(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }
    lv_obj_add_event_cb(screen, ui_gesture_cb, LV_EVENT_GESTURE, nullptr);
}

extern "C" void ui_post_knob_event(void *event)
{
    const int ev = (int)(intptr_t)event;
    s_pending_user_activity.store(true, std::memory_order_release);
    if (ev == KNOB_RIGHT) {
        ui_screen_devices_step_group(1);
    } else if (ev == KNOB_LEFT) {
        ui_screen_devices_step_group(-1);
    }
}

extern "C" void ui_post_button_event(void *event)
{
    const int ev = (int)(intptr_t)event;
    if (ev == BUTTON_SINGLE_CLICK) {
        s_pending_button_click.store(true, std::memory_order_release);
    }
}
