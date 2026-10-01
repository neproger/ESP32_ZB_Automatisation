#include "rgb_led.h"
#include "board.h"

#include "esp_log.h"
#include "esp_check.h"
#include "led_strip.h"

static const char *TAG = "rgb_led";

static led_strip_handle_t s_strip;
static bool s_ready;

static uint8_t clamp_u8(float v)
{
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 255.0f) {
        return 255;
    }
    return (uint8_t)(v + 0.5f);
}

static void send_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ready) {
        return;
    }
    if (r == 0 && g == 0 && b == 0) {
        (void)led_strip_clear(s_strip);
        return;
    }
    (void)led_strip_set_pixel(s_strip, 0, r, g, b);
    (void)led_strip_refresh(s_strip);
}

esp_err_t rgb_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_RGB_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG, "led_strip create failed");
    (void)led_strip_clear(s_strip);
    s_ready = true;
    return ESP_OK;
}

void rgb_led_off(void)
{
    if (!s_ready) {
        return;
    }
    (void)led_strip_clear(s_strip);
}

void rgb_led_set_hs(uint8_t hue, uint8_t sat, uint8_t level)
{
    if (!s_ready) {
        return;
    }
    if (level == 0) {
        rgb_led_off();
        return;
    }
    /* ZCL hue is 0..254 mapped to 0..360 degrees. */
    uint16_t hue360 = (uint16_t)((uint32_t)hue * 360U / 254U);
    (void)led_strip_set_pixel_hsv(s_strip, 0, hue360, sat, level);
    (void)led_strip_refresh(s_strip);
}

void rgb_led_set_xy(uint16_t x16, uint16_t y16, uint8_t level)
{
    if (!s_ready) {
        return;
    }
    if (level == 0) {
        rgb_led_off();
        return;
    }

    const float x = (float)x16 / 65535.0f;
    const float y = (float)y16 / 65535.0f;
    if (y <= 0.001f) {
        rgb_led_off();
        return;
    }

    /* CIE 1931 xy -> sRGB (D65), normalized to Y = 1. */
    const float Y = 1.0f;
    const float X = (Y / y) * x;
    const float Z = (Y / y) * (1.0f - x - y);

    float r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    float b = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;

    float mx = r > g ? r : g;
    if (b > mx) {
        mx = b;
    }
    if (mx > 1.0f) {
        r /= mx;
        g /= mx;
        b /= mx;
    }
    if (r < 0.0f) {
        r = 0.0f;
    }
    if (g < 0.0f) {
        g = 0.0f;
    }
    if (b < 0.0f) {
        b = 0.0f;
    }

    const float scale = (float)level / 254.0f;
    send_rgb(clamp_u8(r * scale * 255.0f), clamp_u8(g * scale * 255.0f), clamp_u8(b * scale * 255.0f));
}
