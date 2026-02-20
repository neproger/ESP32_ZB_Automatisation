#include "lvgl_init.hpp"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_heap_caps.h"

#include "display_init.hpp"
#include "ui_style.hpp"

static const char *TAG_LVGL = "devices_lvgl";

static lv_display_t *s_lvgl_disp = nullptr;
static lv_indev_t *s_lvgl_touch_indev = nullptr;

static constexpr uint32_t kThemePrimaryColorHex = 0x006aa3;
static constexpr uint32_t kThemeSecondaryColorHex = 0x303030;

typedef struct
{
    uint16_t draw_lines;
    bool double_buffer;
    bool buff_dma;
    bool buff_spiram;
    size_t trans_size;
    const char *name;
} lvgl_disp_profile_t;

// SH8601 requires x/y coordinates aligned to 2-pixel boundaries.
// Round LVGL invalidated areas accordingly to avoid visual artifacts.
static void sh8601_lvgl_rounder_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;
    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

static lv_display_t *try_add_display_with_profile(const lvgl_disp_profile_t *p)
{
    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = devices_display_get_panel_io();
    disp_cfg.panel_handle = devices_display_get_panel();
    disp_cfg.buffer_size = LCD_H_RES * p->draw_lines;
    disp_cfg.double_buffer = p->double_buffer;
    disp_cfg.trans_size = p->trans_size;
    disp_cfg.hres = LCD_H_RES;
    disp_cfg.vres = LCD_V_RES;
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.rotation.swap_xy = false;
    disp_cfg.rotation.mirror_x = false;
    disp_cfg.rotation.mirror_y = false;
    disp_cfg.flags.buff_dma = p->buff_dma;
    disp_cfg.flags.buff_spiram = p->buff_spiram;
    disp_cfg.flags.swap_bytes = true;
    disp_cfg.flags.sw_rotate = false;
    return lvgl_port_add_disp(&disp_cfg);
}

esp_err_t devices_lvgl_init(esp_lcd_touch_handle_t touch_handle)
{
    if (s_lvgl_disp)
    {
        return ESP_OK;
    }

    lvgl_port_cfg_t lvgl_cfg = {};
    lvgl_cfg.task_priority = 5;
    lvgl_cfg.task_stack = 7168;
    lvgl_cfg.task_affinity = 1;
    lvgl_cfg.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    lvgl_cfg.task_max_sleep_ms = 10;
    lvgl_cfg.timer_period_ms = 10;
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_LVGL, "LVGL port initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG_LVGL,
             "Heap before LVGL disp: internal=%u, dma=%u, psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const lvgl_disp_profile_t profiles[] = {
        {
            .draw_lines = 8,
            .double_buffer = false,
            .buff_dma = true,
            .buff_spiram = false,
            .trans_size = 0,
            .name = "dma_internal_8lines",
        },
        {
            .draw_lines = 4,
            .double_buffer = false,
            .buff_dma = true,
            .buff_spiram = false,
            .trans_size = 0,
            .name = "dma_internal_4lines",
        },
        {
            .draw_lines = 8,
            .double_buffer = false,
            .buff_dma = false,
            .buff_spiram = true,
            .trans_size = LCD_H_RES, // 1 RGB565 line per DMA transfer (in pixels)
            .name = "psram_draw_8lines",
        },
        {
            .draw_lines = 4,
            .double_buffer = false,
            .buff_dma = false,
            .buff_spiram = true,
            .trans_size = LCD_H_RES, // 1 RGB565 line per DMA transfer (in pixels)
            .name = "psram_draw_4lines",
        },
        {
            .draw_lines = 4,
            .double_buffer = false,
            .buff_dma = true,
            .buff_spiram = false,
            .trans_size = 0,
            .name = "dma_internal_4lines",
        },
    };

    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
    {
        const lvgl_disp_profile_t *p = &profiles[i];
        ESP_LOGW(TAG_LVGL,
                 "Trying LVGL display profile: %s (lines=%u, dbl=%d, dma=%d, psram=%d, trans=%u)",
                 p->name, p->draw_lines, p->double_buffer, p->buff_dma, p->buff_spiram, (unsigned)p->trans_size);
        s_lvgl_disp = try_add_display_with_profile(p);
        if (s_lvgl_disp != nullptr)
        {
            ESP_LOGI(TAG_LVGL, "LVGL display profile selected: %s", p->name);
            break;
        }
    }

    if (s_lvgl_disp == nullptr)
    {
        ESP_LOGE(TAG_LVGL, "lvgl_port_add_disp failed (buffer alloc/config)");
        (void)lvgl_port_deinit();
        return ESP_ERR_NO_MEM;
    }

    if (!lvgl_port_lock(1000))
    {
        ESP_LOGE(TAG_LVGL, "lvgl_port_lock timeout");
        (void)lvgl_port_remove_disp(s_lvgl_disp);
        s_lvgl_disp = nullptr;
        (void)lvgl_port_deinit();
        return ESP_ERR_TIMEOUT;
    }

    // Panel active area is shifted by 6 px in X (see 0x2A init in display_init.cpp).
    // Tell LVGL that logical (0,0) corresponds to physical (6,0).
    lv_display_set_offset(s_lvgl_disp, 6, 0);

    lv_color_t primary = lv_color_hex(kThemePrimaryColorHex);
    lv_color_t secondary = lv_color_hex(kThemeSecondaryColorHex);

    const lv_font_t *font = ui_style::kFontTheme;

    lv_theme_t *theme = lv_theme_default_init(s_lvgl_disp, primary, secondary, true, font);
    lv_display_set_theme(s_lvgl_disp, theme);

    lv_display_add_event_cb(s_lvgl_disp, sh8601_lvgl_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lvgl_port_unlock();

    if (touch_handle)
    {
        lvgl_port_touch_cfg_t touch_cfg = {};
        touch_cfg.disp = s_lvgl_disp;
        touch_cfg.handle = touch_handle;
        s_lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG_LVGL, "LVGL touch input added");
    }
    else
    {
        ESP_LOGW(TAG_LVGL, "LVGL touch not added: no touch handle");
    }

    return ESP_OK;
}

esp_err_t devices_lvgl_deinit(void)
{
    esp_err_t err = lvgl_port_deinit();
    s_lvgl_disp = nullptr;
    s_lvgl_touch_indev = nullptr;
    return err;
}





