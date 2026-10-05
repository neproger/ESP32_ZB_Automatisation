#include "display_p4/display_p4.h"

#include <string.h>

#include "board_p4.h"
#include "st7701_init.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"

#include "display/display.h"

/*
 * Порт Display под плату Guition JC4880P443C_I_W. Рецепт (проверен на железе):
 *   LDO(3, 2500мВ) -> DSI bus (2 lane, 500 Мбит) -> DBI io ->
 *   DPI panel (480x800 RGB565, 34 МГц, h 12/42/42 v 2/8/166) -> reset GPIO5 ->
 *   manual ST7701 init (tx_param) -> panel_init -> backlight GPIO23 (LEDC).
 * Тач GT911: I2C0 SDA7/SCL8, RST3, INT poll, адрес 0x5D/0x14.
 * LVGL — через esp_lvgl_port (DSI display + touch).
 */

static const char *TAG = "display_p4";

static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;
static esp_lcd_panel_handle_t s_panel;
static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_touch_handle_t s_touch;

/* --- подсветка: LEDC PWM на GPIO23 (5 кГц, 8 бит) --- */
#if BOARD_P4_LCD_BL >= 0
#define BL_TIMER LEDC_TIMER_1
#define BL_CHAN LEDC_CHANNEL_1

static bool s_bl_ready;

static void backlight_init(void)
{
    if (s_bl_ready) {
        return;
    }
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BL_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    const ledc_channel_config_t chan = {
        .gpio_num = BOARD_P4_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHAN,
        .timer_sel = BL_TIMER,
        .duty = 255,
        .hpoint = 0,
    };
    ledc_channel_config(&chan);
    s_bl_ready = true;
}

/* 0 — выключено; иначе не ниже ~10%, чтобы панель не гасла в самом низу. */
static void backlight_set(uint8_t duty)
{
    backlight_init();
    if (duty != 0 && duty < 26) {
        duty = 26;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHAN, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHAN);
}
#else
static void backlight_set(uint8_t duty) { (void)duty; }
#endif

/* --- панель --- */

static void panel_reset(void)
{
    if (BOARD_P4_LCD_RST < 0) {
        return;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_P4_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_P4_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_P4_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t send_init_seq(void)
{
    for (size_t i = 0; i < BOARD_P4_PANEL_INIT_CMDS_SIZE; i++) {
        const board_p4_panel_init_cmd_t *e = &board_p4_panel_init_cmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_dbi_io, e->cmd, e->data, e->len), TAG,
                            "init cmd 0x%02X failed", e->cmd);
        if (e->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(e->delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t display_init(void)
{
    esp_ldo_channel_config_t ldo = {
        .chan_id = BOARD_P4_DSI_LDO_CHAN,
        .voltage_mv = BOARD_P4_DSI_LDO_MV,
    };
    esp_ldo_channel_handle_t ldo_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo, &ldo_handle), TAG, "DSI-PHY LDO");

    const esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 500,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus, &s_dsi_bus), TAG, "new_dsi_bus");

    const esp_lcd_dbi_io_config_t dbi = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi, &s_dbi_io), TAG, "new_panel_io_dbi");

    const esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BOARD_P4_LCD_DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_P4_LCD_H_RES,
            .v_size = BOARD_P4_LCD_V_RES,
            .hsync_pulse_width = 12,
            .hsync_back_porch = 42,
            .hsync_front_porch = 42,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 8,
            .vsync_front_porch = 166,
        },
        .flags.disable_lp = 0,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(s_dsi_bus, &dpi, &s_panel), TAG, "new_panel_dpi");

    panel_reset();
    ESP_RETURN_ON_ERROR(send_init_seq(), TAG, "ST7701 init sequence");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init");
    return ESP_OK;
}

/* --- тач GT911 --- */

static esp_err_t touch_init(void)
{
    const i2c_master_bus_config_t bus = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_P4_TOUCH_SDA,
        .scl_io_num = BOARD_P4_TOUCH_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c_bus), TAG, "i2c_new_master_bus");

    if (BOARD_P4_TOUCH_RST >= 0) {
        const gpio_config_t rst = {
            .pin_bit_mask = 1ULL << BOARD_P4_TOUCH_RST,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst);
        gpio_set_level(BOARD_P4_TOUCH_RST, 0);
        esp_rom_delay_us(10 * 1000);
        gpio_set_level(BOARD_P4_TOUCH_RST, 1);
        esp_rom_delay_us(50 * 1000);
    }

    uint8_t addr = 0;
    if (i2c_master_probe(s_i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) == ESP_OK) {
        addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    } else if (i2c_master_probe(s_i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK) {
        addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    } else {
        ESP_LOGW(TAG, "GT911 нет на 0x5D/0x14 — панель без тача");
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t io = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io.dev_addr = addr;
    io.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t touch_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io, &touch_io), TAG, "touch io");

    const esp_lcd_touch_config_t cfg = {
        .x_max = BOARD_P4_LCD_H_RES,
        .y_max = BOARD_P4_LCD_V_RES,
        .rst_gpio_num = BOARD_P4_TOUCH_RST,
        .int_gpio_num = BOARD_P4_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(touch_io, &cfg, &s_touch), TAG, "gt911 new");
    ESP_LOGI(TAG, "GT911 на 0x%02X", addr);
    return ESP_OK;
}

/* --- LVGL --- */

static lv_display_t *lvgl_start(void)
{
    const lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port) != ESP_OK) {
        return NULL;
    }

    const lvgl_port_display_cfg_t disp = {
        .io_handle = s_dbi_io,
        .panel_handle = s_panel,
        .buffer_size = BOARD_P4_LCD_H_RES * 60,
        .double_buffer = false,
        .hres = BOARD_P4_LCD_H_RES,
        .vres = BOARD_P4_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 0,
            .buff_spiram = 1,
            .sw_rotate = 0,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi = {
        .flags = {.avoid_tearing = 0},
    };
    lv_display_t *lv_disp = lvgl_port_add_disp_dsi(&disp, &dsi);
    if (lv_disp == NULL) {
        return NULL;
    }

    if (s_touch != NULL) {
        const lvgl_port_touch_cfg_t touch = {
            .disp = lv_disp,
            .handle = s_touch,
            .scale = {.x = 1.0f, .y = 1.0f},
        };
        (void)lvgl_port_add_touch(&touch);
    }
    return lv_disp;
}

void display_p4_start(domain_t *domain)
{
    if (domain == NULL) {
        return;
    }
    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "панель не поднялась");
        return;
    }
    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "тач недоступен — UI без ввода");
    }
    if (lvgl_start() == NULL) {
        ESP_LOGE(TAG, "LVGL не стартовал");
        return;
    }

    backlight_set(255);

    if (lvgl_port_lock(0)) {
        display_start(domain);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "display порт запущен");
}
