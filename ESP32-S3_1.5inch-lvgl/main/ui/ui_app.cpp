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

#include "ui_events_bridge.hpp"
#include "ui_screen_devices.hpp"
#include "ui_store.hpp"

namespace
{
static const char *TAG_UI = "ui_app";

static ui_store_t *s_store = nullptr;
static bool s_render_requested = false;
static uint64_t s_last_render_ms = 0;
static constexpr uint32_t kMinRenderIntervalMs = 150;

void request_render()
{
    s_render_requested = true;
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
            ui_screen_devices_apply_state_event(s_store, &events[i]);
        }
    }

    if (s_render_requested)
    {
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
    ui_screen_devices_render(s_store);
    s_last_render_ms = (uint64_t)(esp_timer_get_time() / 1000);
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
    (void)ev;
}
