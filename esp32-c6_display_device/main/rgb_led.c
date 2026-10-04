#include "rgb_led.h"
#include "board.h"

#include <math.h>

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
        /* На этой плате каналы WS2812 идут R,G,B (проверено: GRB давал R<->G swap). */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
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

void rgb_led_set_ct(uint16_t mireds, uint8_t level)
{
    if (!s_ready) {
        return;
    }
    if (level == 0) {
        rgb_led_off();
        return;
    }

    /* Clamp to a sensible 2000K..10000K window. */
    if (mireds < 100) {
        mireds = 100;
    }
    if (mireds > 500) {
        mireds = 500;
    }

    /* Mireds -> Kelvin, then the Tanner Helland approximation of a
       black-body radiator to sRGB. */
    const float kelvin = 1000000.0f / (float)mireds;
    const float t = kelvin / 100.0f;

    float r;
    float g;
    float b;
    if (t <= 66.0f) {
        r = 255.0f;
        g = 99.4708025861f * logf(t) - 161.1195681661f;
        b = (t <= 19.0f) ? 0.0f : (138.5177312231f * logf(t - 10.0f) - 305.0447927307f);
    } else {
        r = 329.698727446f * powf(t - 60.0f, -0.1332047592f);
        g = 288.1221695283f * powf(t - 60.0f, -0.0755148492f);
        b = 255.0f;
    }

    const float scale = (float)level / 254.0f;
    send_rgb(clamp_u8(r * scale), clamp_u8(g * scale), clamp_u8(b * scale));
}
