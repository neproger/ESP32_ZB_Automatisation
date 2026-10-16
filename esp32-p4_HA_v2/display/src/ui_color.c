#include "ui_color.h"

#include <math.h>

/* --- sRGB <-> linear <-> HSV <-> CIE xy --- */

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float c)
{
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

float ui_color_clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

lv_color_t ui_color_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return lv_color_hex(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

void ui_color_rgb8_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8, float *h, float *s, float *v)
{
    const float r = r8 / 255.0f, g = g8 / 255.0f, b = b8 / 255.0f;
    const float mx = fmaxf(r, fmaxf(g, b));
    const float mn = fminf(r, fminf(g, b));
    const float d = mx - mn;
    *v = mx;
    *s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) {
        *h = 0.0f;
        return;
    }
    float deg;
    if (mx == r) {
        deg = 60.0f * fmodf((g - b) / d, 6.0f);
    } else if (mx == g) {
        deg = 60.0f * ((b - r) / d + 2.0f);
    } else {
        deg = 60.0f * ((r - g) / d + 4.0f);
    }
    if (deg < 0.0f) {
        deg += 360.0f;
    }
    *h = deg;
}

void ui_color_hsv_to_rgb8(float h, float s, float v, uint8_t *r8, uint8_t *g8, uint8_t *b8)
{
    const float c = v * s;
    const float hp = fmodf(h, 360.0f) / 60.0f;
    const float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hp < 1.0f) {
        r = c;
        g = x;
    } else if (hp < 2.0f) {
        r = x;
        g = c;
    } else if (hp < 3.0f) {
        g = c;
        b = x;
    } else if (hp < 4.0f) {
        g = x;
        b = c;
    } else if (hp < 5.0f) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    const float m = v - c;
    *r8 = (uint8_t)((r + m) * 255.0f + 0.5f);
    *g8 = (uint8_t)((g + m) * 255.0f + 0.5f);
    *b8 = (uint8_t)((b + m) * 255.0f + 0.5f);
}

void ui_color_rgb8_to_xy(uint8_t r8, uint8_t g8, uint8_t b8, uint16_t *x_out, uint16_t *y_out)
{
    const float r = srgb_to_linear(r8 / 255.0f);
    const float g = srgb_to_linear(g8 / 255.0f);
    const float b = srgb_to_linear(b8 / 255.0f);
    const float X = r * 0.4124f + g * 0.3576f + b * 0.1805f;
    const float Y = r * 0.2126f + g * 0.7152f + b * 0.0722f;
    const float Z = r * 0.0193f + g * 0.1192f + b * 0.9505f;
    const float sum = X + Y + Z;
    const float x = sum > 0.0f ? X / sum : 0.0f;
    const float y = sum > 0.0f ? Y / sum : 0.0f;
    *x_out = (uint16_t)(ui_color_clamp01f(x) * 65535.0f + 0.5f);
    *y_out = (uint16_t)(ui_color_clamp01f(y) * 65535.0f + 0.5f);
}

void ui_color_xy_to_rgb8(uint16_t x16, uint16_t y16, uint8_t *r8, uint8_t *g8, uint8_t *b8)
{
    const float x = x16 / 65535.0f;
    const float y = y16 / 65535.0f;
    if (y <= 0.0001f) {
        *r8 = *g8 = *b8 = 255;
        return;
    }
    const float X = x / y;
    const float Z = (1.0f - x - y) / y;
    float r = X * 3.2406f - 1.5372f - Z * 0.4986f;
    float g = -X * 0.9689f + 1.8758f + Z * 0.0415f;
    float b = X * 0.0557f - 0.204f + Z * 1.057f;
    float m = fmaxf(r, fmaxf(g, b));
    if (m < 1.0f) {
        m = 1.0f;
    }
    *r8 = (uint8_t)(ui_color_clamp01f(linear_to_srgb(r / m)) * 255.0f + 0.5f);
    *g8 = (uint8_t)(ui_color_clamp01f(linear_to_srgb(g / m)) * 255.0f + 0.5f);
    *b8 = (uint8_t)(ui_color_clamp01f(linear_to_srgb(b / m)) * 255.0f + 0.5f);
}

lv_color_t ui_color_from_hs(uint16_t hue, uint8_t sat)
{
    uint8_t r, g, b;
    ui_color_hsv_to_rgb8((float)(hue % 360u), (float)sat / 100.0f, 1.0f, &r, &g, &b);
    return ui_color_from_rgb(r, g, b);
}
