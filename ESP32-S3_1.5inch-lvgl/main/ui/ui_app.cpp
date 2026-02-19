#include "ui_app.hpp"

#include <cstdint>
#include <cstring>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "iot_knob.h"

#include "devices_init.h"
#include "ui_control_ack.hpp"
#include "ui_events_bridge.hpp"
#include "ui_screen_devices.hpp"
#include "ui_screen_saver.hpp"
#include "ui_store.hpp"
#include "ui_style.hpp"
#include "ui_widgets.hpp"

namespace
{
static const char *TAG_UI = "ui_app";

static ui_store_t *s_store = nullptr;
static bool s_render_requested = false;
static uint64_t s_last_render_ms = 0;
static bool s_display_enabled = true;
static bool s_ui_ready = false;
static bool s_saver_active = false;
static lv_obj_t *s_splash = nullptr;
static constexpr uint8_t kDisplayBrightness80Pct = 204;
static constexpr uint32_t kMinRenderIntervalMs = 150;
static constexpr uint32_t kControlAckTimeoutMs = 1800;
static constexpr uint32_t kScreensaverTimeoutMs = 4000;

void request_render()
{
    s_render_requested = true;
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
    request_render();
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
    if (!s_store) {
        return;
    }
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
    bool changed = false;
    if (dir == LV_DIR_BOTTOM) {
        changed = ui_store_prev_item(s_store);
    } else if (dir == LV_DIR_TOP) {
        changed = ui_store_next_item(s_store);
    }

    if (changed) {
        request_render();
    }
}

void ui_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    ui_control_ack_poll_timeouts(now_ms, kControlAckTimeoutMs);

    if (!s_ui_ready) {
        s_ui_ready = true;
        splash_show(false);
        s_saver_active = false;
        s_last_render_ms = 0;
        request_render();
        note_user_activity();
    }

    gw_event_t events[8] = {};
    const size_t n = ui_events_bridge_drain(events, 8);
    for (size_t i = 0; i < n; ++i)
    {
        if (!s_store)
        {
            continue;
        }

        const bool structural =
            (strcmp(events[i].type, "device.join") == 0) ||
            (strcmp(events[i].type, "device.leave") == 0) ||
            (strcmp(events[i].type, "device.changed") == 0) ||
            (strcmp(events[i].type, "device.update") == 0) ||
            (strcmp(events[i].type, "group.changed") == 0);

        if (structural)
        {
            if (ui_store_apply_event(s_store, &events[i]))
            {
                s_render_requested = true;
            }
        }
        else
        {
            (void)ui_store_apply_event(s_store, &events[i]);
            if (s_ui_ready && !s_saver_active) {
                ui_screen_devices_apply_state_event(s_store, &events[i]);
            }
        }
    }

    if (s_store)
    {
        const ui_group_item_vm_t *item = ui_store_active_item(s_store);
        if (item)
        {
            ui_widgets_refresh_ack(item->uid.uid, item->endpoint_id);
        }
    }

    lv_display_t *display = lv_display_get_default();
    const uint32_t inactive_ms = display ? lv_display_get_inactive_time(display) : 0;
    if (s_ui_ready && !s_saver_active && inactive_ms >= kScreensaverTimeoutMs)
    {
        s_saver_active = true;
        ui_screen_saver_show(true);
    }
    ui_screen_saver_tick();

    if (s_render_requested)
    {
        if (!s_ui_ready || s_saver_active) {
            return;
        }
        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (s_last_render_ms != 0 && (now_ms - s_last_render_ms) < kMinRenderIntervalMs)
        {
            return;
        }
        if (s_store)
        {
            ui_screen_devices_render(s_store);
        }
        s_last_render_ms = now_ms;
        s_render_requested = false;
    }
}
} // namespace

void ui_app_init(void)
{
    if (!s_store)
    {
        s_store = (ui_store_t *)heap_caps_malloc(sizeof(ui_store_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_store)
        {
            s_store = (ui_store_t *)heap_caps_malloc(sizeof(ui_store_t), MALLOC_CAP_8BIT);
        }
        ESP_ERROR_CHECK(s_store ? ESP_OK : ESP_ERR_NO_MEM);
    }

    lvgl_port_lock(-1);

    ui_store_init(s_store);
    ui_store_reload(s_store);

    lv_obj_t *scr = lv_screen_active();
    ui_screen_devices_init(scr);
    lv_obj_add_event_cb(scr, ui_gesture_cb, LV_EVENT_GESTURE, nullptr);
    ui_screen_saver_init(scr, wake_from_screensaver);
    splash_init(scr);
    splash_show(true);
    s_ui_ready = false;
    s_last_render_ms = 0;
    lv_timer_create(ui_tick_cb, 100, nullptr);

    lvgl_port_unlock();

    ESP_ERROR_CHECK(ui_events_bridge_init());
    ESP_LOGI(TAG_UI, "Group UI initialized");
}

extern "C" void LVGL_knob_event(void *event)
{
    if (!s_store)
    {
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
    const int ev = (int)(intptr_t)event;
    if (ev == KNOB_RIGHT)
    {
        if (ui_store_next_group(s_store))
        {
            request_render();
        }
    }
    else if (ev == KNOB_LEFT)
    {
        if (ui_store_prev_group(s_store))
        {
            request_render();
        }
    }
}

extern "C" void LVGL_button_event(void *event)
{
    const int ev = (int)(intptr_t)event;
    if (ev != BUTTON_SINGLE_CLICK)
    {
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
}
