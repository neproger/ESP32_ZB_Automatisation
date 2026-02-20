#include "ui_screen_saver.hpp"

#include <cstdint>
#include <cstdio>
#include <time.h>

#include "gw_core/net_time.h"
#include "gw_core/state_store.h"
#include "s3_weather_service.h"
#include "ui_style.hpp"

namespace
{
lv_obj_t *s_overlay = nullptr;
lv_obj_t *s_stack = nullptr;
lv_obj_t *s_time = nullptr;
lv_obj_t *s_location = nullptr;
lv_obj_t *s_weather = nullptr;
ui_screen_saver_wakeup_cb_t s_wakeup_cb = nullptr;
uint64_t s_last_rendered_second = UINT64_MAX;
uint64_t s_last_weather_ts = UINT64_MAX;
char s_last_location[64] = {};

static const gw_device_uid_t kWeatherUid = {"0xWEATHER000000001"};
static constexpr uint8_t kWeatherEndpoint = 1;

bool load_weather_value_f32(const char *key, float *out)
{
    if (!key || !out) {
        return false;
    }
    gw_state_item_t st = {};
    if (gw_state_store_get(&kWeatherUid, kWeatherEndpoint, key, &st) != ESP_OK) {
        return false;
    }
    if (st.value_type != GW_STATE_VALUE_F32) {
        return false;
    }
    *out = st.value_f32;
    return true;
}

bool load_weather_value_u32(const char *key, uint32_t *out)
{
    if (!key || !out) {
        return false;
    }
    gw_state_item_t st = {};
    if (gw_state_store_get(&kWeatherUid, kWeatherEndpoint, key, &st) != ESP_OK) {
        return false;
    }
    if (st.value_type != GW_STATE_VALUE_U32) {
        return false;
    }
    *out = st.value_u32;
    return true;
}

bool load_weather_value_u64(const char *key, uint64_t *out)
{
    if (!key || !out) {
        return false;
    }
    gw_state_item_t st = {};
    if (gw_state_store_get(&kWeatherUid, kWeatherEndpoint, key, &st) != ESP_OK) {
        return false;
    }
    if (st.value_type != GW_STATE_VALUE_U64) {
        return false;
    }
    *out = st.value_u64;
    return true;
}

void refresh_location_if_needed(void)
{
    if (!s_location) {
        return;
    }

    char loc[64] = {};
    s3_weather_service_get_location(loc, sizeof(loc));
    if (strncmp(loc, s_last_location, sizeof(s_last_location)) == 0) {
        return;
    }
    strlcpy(s_last_location, loc, sizeof(s_last_location));
    if (loc[0] == '\0') {
        lv_label_set_text(s_location, "Location: --");
    } else {
        lv_label_set_text(s_location, loc);
    }
}

void refresh_weather_subtitle_if_needed(void)
{
    if (!s_weather) {
        return;
    }
    uint64_t updated_ms = 0;
    if (!load_weather_value_u64("weather_updated_ms", &updated_ms)) {
        if (s_last_weather_ts != 0) {
            lv_label_set_text(s_weather, "--.-C  --%");
            s_last_weather_ts = 0;
        }
        return;
    }
    if (updated_ms == s_last_weather_ts) {
        return;
    }

    float temp_c = 0.0f;
    float hum_pct = 0.0f;
    uint32_t code = 0;
    const bool has_temp = load_weather_value_f32("weather_temp_c", &temp_c);
    const bool has_hum = load_weather_value_f32("weather_humidity_pct", &hum_pct);
    const bool has_code = load_weather_value_u32("weather_code", &code);
    (void)code;

    if (!has_temp || !has_hum || !has_code) {
        lv_label_set_text(s_weather, "--.-C  --%");
        s_last_weather_ts = updated_ms;
        return;
    }

    char line[48] = {};
    (void)snprintf(line, sizeof(line), "%.1fC  %.0f%%", (double)temp_c, (double)hum_pct);
    lv_label_set_text(s_weather, line);
    s_last_weather_ts = updated_ms;
}

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

    s_location = lv_label_create(s_stack);
    lv_label_set_text(s_location, "Location: --");
    lv_obj_set_style_text_color(s_location, lv_color_hex(ui_style::kSubtitleTextHex), 0);
    lv_obj_set_style_text_font(s_location, ui_style::kFontSubtitle, 0);

    s_weather = lv_label_create(s_stack);
    lv_label_set_text(s_weather, "--.-C  --%");
    lv_obj_set_style_text_color(s_weather, lv_color_hex(ui_style::kTitleTextHex), 0);
    lv_obj_set_style_text_font(s_weather, ui_style::kFontSubtitle, 0);

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
        refresh_location_if_needed();
        refresh_weather_subtitle_if_needed();
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
    refresh_location_if_needed();
    refresh_weather_subtitle_if_needed();
}
