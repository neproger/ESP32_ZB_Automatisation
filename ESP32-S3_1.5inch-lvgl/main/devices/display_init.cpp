#include "display_init.hpp"

#include "esp_log.h"
#include "esp_lcd_sh8601.h"
#include "driver/ledc.h"

#include "devices_init.h"

static const char *TAG_DISPLAY = "devices_display";

// LCD IO and panel handles shared with other modules via accessors
static esp_lcd_panel_io_handle_t s_lcd_io = nullptr;
static esp_lcd_panel_handle_t s_lcd_panel = nullptr;

// Backlight PWM (LEDC)
static bool s_bk_ledc_inited = false;
static ledc_mode_t s_bk_ledc_mode = LEDC_LOW_SPEED_MODE;
static ledc_timer_t s_bk_ledc_timer = LEDC_TIMER_0;
static ledc_channel_t s_bk_ledc_channel = LEDC_CHANNEL_0;

// Panel vendor init commands (SH8601)
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 0, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 0, 10},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x51, (uint8_t[]){0xA0}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    // Column address set: start X=0, end X=477 (478 px, matches LCD_H_RES)
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xDD}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 60},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

esp_err_t devices_display_init(void)
{
    if (s_lcd_panel)
    {
        return ESP_OK;
    }

    // Backlight init (LEDC)
    if (PIN_BK_LIGHT >= 0 && !s_bk_ledc_inited)
    {
        ledc_timer_config_t tcfg = {};
        tcfg.speed_mode = s_bk_ledc_mode;
        tcfg.duty_resolution = LEDC_TIMER_8_BIT;
        tcfg.timer_num = s_bk_ledc_timer;
        tcfg.freq_hz = 5000;
        tcfg.clk_cfg = LEDC_AUTO_CLK;
        esp_err_t err = ledc_timer_config(&tcfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_DISPLAY, "Backlight LEDC timer config failed: %s", esp_err_to_name(err));
            return err;
        }

        ledc_channel_config_t ccfg = {};
        ccfg.gpio_num = PIN_BK_LIGHT;
        ccfg.speed_mode = s_bk_ledc_mode;
        ccfg.channel = s_bk_ledc_channel;
        ccfg.intr_type = LEDC_INTR_DISABLE;
        ccfg.timer_sel = s_bk_ledc_timer;
        ccfg.duty = 0;
        ccfg.hpoint = 0;
        ccfg.flags.output_invert = 0;

        err = ledc_channel_config(&ccfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_DISPLAY, "Backlight LEDC channel config failed: %s", esp_err_to_name(err));
            return err;
        }

        err = ledc_set_duty(s_bk_ledc_mode, s_bk_ledc_channel, 230);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_DISPLAY, "Backlight LEDC set duty failed: %s", esp_err_to_name(err));
            return err;
        }
        err = ledc_update_duty(s_bk_ledc_mode, s_bk_ledc_channel);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_DISPLAY, "Backlight LEDC update duty failed: %s", esp_err_to_name(err));
            return err;
        }

        s_bk_ledc_inited = true;
    }

    ESP_LOGI(TAG_DISPLAY, "Initialize SPI/QSPI bus");
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = PIN_LCD_PCLK;
    buscfg.data0_io_num = PIN_LCD_DATA0;
    buscfg.data1_io_num = PIN_LCD_DATA1;
    buscfg.data2_io_num = PIN_LCD_DATA2;
    buscfg.data3_io_num = PIN_LCD_DATA3;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8;
    esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DISPLAY, "SPI bus initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG_DISPLAY, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = -1;
    io_config.cs_gpio_num = PIN_LCD_CS;
    io_config.pclk_hz = LCD_SPI_SPEED_MHZ * 1000 * 1000;
    io_config.trans_queue_depth = LCD_TRANS_QUEUE_DEPTH;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.flags.quad_mode = true;

    sh8601_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(sh8601_lcd_init_cmd_t);
    vendor_config.flags.use_qspi_interface = 1;

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_lcd_io);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DISPLAY, "New panel IO SPI failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;

    ESP_LOGI(TAG_DISPLAY, "Install LCD driver");
    err = esp_lcd_new_panel_sh8601(s_lcd_io, &panel_config, &s_lcd_panel);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DISPLAY, "New SH8601 panel failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_reset(s_lcd_panel);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DISPLAY, "LCD panel reset failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_init(s_lcd_panel);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DISPLAY, "LCD panel init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_disp_on_off(s_lcd_panel, true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DISPLAY, "LCD panel display on failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t devices_display_deinit(void)
{
    esp_err_t err = ESP_OK;

    if (s_lcd_panel)
    {
        (void)esp_lcd_panel_disp_on_off(s_lcd_panel, false);
        esp_err_t e = esp_lcd_panel_del(s_lcd_panel);
        if (err == ESP_OK && e != ESP_OK)
        {
            err = e;
        }
        s_lcd_panel = nullptr;
    }

    if (s_lcd_io)
    {
        esp_err_t e = esp_lcd_panel_io_del(s_lcd_io);
        if (err == ESP_OK && e != ESP_OK)
        {
            err = e;
        }
        s_lcd_io = nullptr;
    }

    if (s_bk_ledc_inited)
    {
        ledc_stop(s_bk_ledc_mode, s_bk_ledc_channel, 0);
        s_bk_ledc_inited = false;
    }

    (void)spi_bus_free(LCD_HOST);

    return err;
}

esp_err_t devices_display_set_brightness(uint8_t level)
{
    if (!s_bk_ledc_inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ledc_set_duty(s_bk_ledc_mode, s_bk_ledc_channel, level);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DISPLAY, "Backlight set duty failed: %s", esp_err_to_name(err));
        return err;
    }
    err = ledc_update_duty(s_bk_ledc_mode, s_bk_ledc_channel);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_DISPLAY, "Backlight update duty failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t devices_display_get_brightness(uint8_t *out_level)
{
    if (!out_level)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_bk_ledc_inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t duty = ledc_get_duty(s_bk_ledc_mode, s_bk_ledc_channel);
    *out_level = (uint8_t)duty;
    return ESP_OK;
}

esp_lcd_panel_io_handle_t devices_display_get_panel_io(void)
{
    return s_lcd_io;
}

esp_lcd_panel_handle_t devices_display_get_panel(void)
{
    return s_lcd_panel;
}
