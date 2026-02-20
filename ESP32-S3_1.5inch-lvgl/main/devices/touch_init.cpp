#include "touch_init.hpp"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_panel_io.h"

#include "display_init.hpp"

static const char *TAG_TOUCH = "devices_touch";

// Touch controller configuration (CST816S over I2C)
#define TOUCH_I2C_NUM I2C_NUM_0
#define TOUCH_I2C_CLK_HZ (400000)
#define PIN_TOUCH_SCL (GPIO_NUM_3)
#define PIN_TOUCH_SDA (GPIO_NUM_1)
#define PIN_TOUCH_RST (GPIO_NUM_2)
#define PIN_TOUCH_INT (GPIO_NUM_4)

static esp_lcd_touch_handle_t s_touch_handle = NULL;
static esp_lcd_panel_io_handle_t s_tp_io_handle = NULL;
static bool s_touch_inited = false;

esp_err_t devices_touch_init(esp_lcd_touch_handle_t *out_handle)
{
    if (s_touch_inited)
    {
        if (out_handle)
        {
            *out_handle = s_touch_handle;
        }
        return ESP_OK;
    }

    i2c_config_t i2c_conf = {};
    i2c_conf.mode = I2C_MODE_MASTER;
    i2c_conf.sda_io_num = PIN_TOUCH_SDA;
    i2c_conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.scl_io_num = PIN_TOUCH_SCL;
    i2c_conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.master.clk_speed = TOUCH_I2C_CLK_HZ;
    esp_err_t err = i2c_param_config(TOUCH_I2C_NUM, &i2c_conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_TOUCH, "I2C configuration failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2c_driver_install(TOUCH_I2C_NUM, i2c_conf.mode, 0, 0, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_TOUCH, "I2C initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_config_t int_gpio_cfg = {};
    int_gpio_cfg.pin_bit_mask = 1ULL << PIN_TOUCH_INT;
    int_gpio_cfg.mode = GPIO_MODE_INPUT;
    int_gpio_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    int_gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    int_gpio_cfg.intr_type = GPIO_INTR_DISABLE;
    err = gpio_config(&int_gpio_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_TOUCH, "INT GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = LCD_H_RES;
    tp_cfg.y_max = LCD_V_RES;
    tp_cfg.rst_gpio_num = PIN_TOUCH_RST;
    tp_cfg.int_gpio_num = PIN_TOUCH_INT;
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy = 0;
    tp_cfg.flags.mirror_x = 0;
    tp_cfg.flags.mirror_y = 0;

#ifdef ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG
    // Legacy panel-io i2c driver takes clock from i2c_param_config().
    // Do not set scl_speed_hz in panel-io config.
    esp_lcd_panel_io_i2c_config_t tp_io_config = {};
    tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
    tp_io_config.on_color_trans_done = NULL;
    tp_io_config.user_ctx = NULL;
    tp_io_config.control_phase_bytes = 1;
    tp_io_config.dc_bit_offset = 0;
    tp_io_config.lcd_cmd_bits = 8;
    tp_io_config.lcd_param_bits = 8;
    tp_io_config.flags.dc_low_on_data = 0;
    tp_io_config.flags.disable_control_phase = 1;
    err = esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_I2C_NUM, &tp_io_config, &s_tp_io_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_TOUCH, "new_panel_io_i2c failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_touch_new_i2c_cst816s(s_tp_io_handle, &tp_cfg, &s_touch_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_TOUCH, "touch_new_i2c_cst816s failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG_TOUCH, "CST816S touch initialized on I2C0 (SDA=%d, SCL=%d, INT=%d, RST=%d)",
             PIN_TOUCH_SDA, PIN_TOUCH_SCL, PIN_TOUCH_INT, PIN_TOUCH_RST);

    s_touch_inited = true;
    if (out_handle)
    {
        *out_handle = s_touch_handle;
    }

    return ESP_OK;
#else
    ESP_LOGW(TAG_TOUCH, "CST816S touch driver not available, skipping touch init");
    s_touch_handle = NULL;
    s_tp_io_handle = NULL;
    s_touch_inited = false;
    return ESP_OK;
#endif
}

esp_err_t devices_touch_deinit(void)
{
    esp_err_t err = ESP_OK;

    if (s_touch_inited && s_touch_handle)
    {
        esp_err_t e = esp_lcd_touch_del(s_touch_handle);
        if (err == ESP_OK && e != ESP_OK)
        {
            err = e;
        }
        s_touch_handle = NULL;
    }

    if (s_tp_io_handle)
    {
        esp_err_t e = esp_lcd_panel_io_del(s_tp_io_handle);
        if (err == ESP_OK && e != ESP_OK)
        {
            err = e;
        }
        s_tp_io_handle = NULL;
    }

    (void)i2c_driver_delete(TOUCH_I2C_NUM);
    s_touch_inited = false;

    return err;
}




