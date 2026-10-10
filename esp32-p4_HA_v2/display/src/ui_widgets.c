#include "ui_widgets.h"

#include <math.h>
#include <stdio.h>

#include "semantics/semantics.h"
#include "sys/sys_error.h"
#include "ui_commands.h"
#include "ui_compat.h"
#include "ui_icons.h"
#include "ui_style.h"

struct ui_widget {
    ui_widget_kind_t kind;
    ha_property_id_t property; /* семантика состояния (через мост) */
    domain_t *domain;
    ha_zb_state_key_t state;

    lv_obj_t *obj;         /* корневой объект виджета */
    lv_obj_t *slider;      /* LEVEL / COLOR_TEMP */
    lv_obj_t *value_label; /* подпись уровня/значения/индикатора */

    /* Цвет: превью + слайдеры оттенка/насыщенности; яркость — общий slider/value_label. */
    lv_obj_t *swatch;     /* кружок текущего цвета */
    lv_obj_t *hue_slider; /* оттенок 0..359 (радужный градиент) */
    lv_obj_t *hue_label;
    lv_obj_t *sat_slider; /* насыщенность 0..100 */
    lv_obj_t *sat_label;

    lv_timer_t *cooldown; /* SWITCH: снять DISABLED после нажатия/таймаута */
    bool has_last;        /* SWITCH: есть сохранённое значение из репорта */
    uint32_t last_raw;
};

/* Форма виджета — по семантике свойства, а не по ZCL-координате. */
ui_widget_kind_t ui_widget_kind_for(ha_property_id_t property)
{
    switch (property) {
    case HA_PROPERTY_POWER:
        return UI_WIDGET_SWITCH;
    case HA_PROPERTY_BRIGHTNESS:
        return UI_WIDGET_LEVEL;
    case HA_PROPERTY_COLOR_TEMPERATURE:
        return UI_WIDGET_COLOR_TEMP;
    case HA_PROPERTY_COLOR_HUE:
    case HA_PROPERTY_COLOR_SATURATION:
    case HA_PROPERTY_COLOR_X:
    case HA_PROPERTY_COLOR_Y:
        return UI_WIDGET_COLOR;
    case HA_PROPERTY_OCCUPANCY:
        return UI_WIDGET_INDICATOR;
    default:
        return UI_WIDGET_VALUE;
    }
}

/* Прочитать состояние по ключу и декодировать в семантику (через мост). */
static bool decode_state(domain_t *domain, const ha_zb_state_key_t *key, ha_value_t *out)
{
    ha_zb_state_record_t record = {0};
    if (sys_failed(domain_entity_get(domain, (domain_entity_t)HA_ENTITY_STATE, key, &record))) {
        return false;
    }
    return semantics_state_value(key, &record, out);
}

/* Текст значения по единице свойства. */
static void format_property(ha_property_id_t property, const ha_value_t *value, char *out,
                            size_t out_size)
{
    double number = 0.0;
    if (!semantics_value_to_double(value, &number)) {
        snprintf(out, out_size, "—");
        return;
    }
    switch (ha_property_desc(property)->unit) {
    case HA_UNIT_CELSIUS:
        snprintf(out, out_size, "%.1f °C", number);
        break;
    case HA_UNIT_PERCENT:
        snprintf(out, out_size, "%.1f %%", number);
        break;
    case HA_UNIT_LUX:
        snprintf(out, out_size, "%.0f lx", number);
        break;
    case HA_UNIT_VOLT:
        snprintf(out, out_size, "%.1f V", number);
        break;
    case HA_UNIT_KELVIN:
        snprintf(out, out_size, "%.0f K", number);
        break;
    default:
        snprintf(out, out_size, "%.0f", number);
        break;
    }
}

/* --- создание объектов --- */

lv_obj_t *ui_slider_create_styled(lv_obj_t *parent, int min, int max)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_height(slider, 10); /* толщина дорожки; маркер вырастает pad'ом */
    lv_slider_set_range(slider, min, max);

    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_CHIP), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(slider, 0, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
    /* Палец не «теряет» слайдер при небольшом уходе в сторону. */
    lv_obj_add_flag(slider, LV_OBJ_FLAG_PRESS_LOCK);
    return slider;
}

/*
 * Выпадающий список в общем стиле. Стрелка — встроенный символ dropdown со шрифтом ui_icons
 * (вместо LV_SYMBOL_DOWN, которого нет в шрифтах UI — иначе «тофу»). Символ рисуется стилем
 * LV_PART_INDICATOR; pad_column задаёт зазор между текстом и стрелкой, а наличие символа
 * заставляет dropdown выравнивать текст влево (без символа он центрируется и налезает).
 */
lv_obj_t *ui_dropdown_create_styled(lv_obj_t *parent)
{
    lv_obj_t *dropdown = lv_dropdown_create(parent);
    lv_obj_set_style_text_font(dropdown, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_column(dropdown, 12, LV_PART_MAIN); /* зазор текст ↔ стрелка */
    lv_obj_set_style_pad_right(dropdown, 12, LV_PART_MAIN);  /* отступ стрелки от края */
    lv_obj_set_style_text_font(dropdown, &ui_icons, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(UI_COL_TEXT), LV_PART_INDICATOR);
    lv_dropdown_set_symbol(dropdown, UI_ICON_CARET_DOWN);
    return dropdown;
}

static lv_obj_t *make_slider_row(lv_obj_t *parent, int min, int max, lv_obj_t **out_slider,
                                 lv_obj_t **out_label)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 36); /* вмещает круглый маркер (тонкая дорожка + pad) */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    ui_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    *out_slider = ui_slider_create_styled(row, min, max);
    lv_obj_set_flex_grow(*out_slider, 1);

    *out_label = lv_label_create(row);
    lv_obj_set_style_text_font(*out_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(*out_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_width(*out_label, 96);
    lv_obj_set_style_text_align(*out_label, LV_TEXT_ALIGN_RIGHT, 0);
    return row;
}

static lv_obj_t *make_indicator(lv_obj_t *parent, ui_widget_t *widget)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_size(pill, 220, 56);
    lv_obj_set_style_radius(pill, 28, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(UI_COL_CHIP), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    ui_set_scrollable(pill, false);

    widget->value_label = lv_label_create(pill);
    lv_obj_set_style_text_font(widget->value_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(widget->value_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(widget->value_label);
    return pill;
}

/* --- цвет: sRGB <-> HSV <-> CIE xy (для MoveToColor) --- */

static lv_color_t color_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return lv_color_hex(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

static float clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float c)
{
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static void rgb8_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8, float *h, float *s, float *v)
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

static void hsv_to_rgb8(float h, float s, float v, uint8_t *r8, uint8_t *g8, uint8_t *b8)
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

static void rgb8_to_xy(uint8_t r8, uint8_t g8, uint8_t b8, uint16_t *x_out, uint16_t *y_out)
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
    *x_out = (uint16_t)(clamp01f(x) * 65535.0f + 0.5f);
    *y_out = (uint16_t)(clamp01f(y) * 65535.0f + 0.5f);
}

static void xy_to_rgb8(uint16_t x16, uint16_t y16, uint8_t *r8, uint8_t *g8, uint8_t *b8)
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
    *r8 = (uint8_t)(clamp01f(linear_to_srgb(r / m)) * 255.0f + 0.5f);
    *g8 = (uint8_t)(clamp01f(linear_to_srgb(g / m)) * 255.0f + 0.5f);
    *b8 = (uint8_t)(clamp01f(linear_to_srgb(b / m)) * 255.0f + 0.5f);
}

/* Радужная дорожка слайдера оттенка (7 стопов; CONFIG_LV_GRADIENT_MAX_STOPS>=7). */
static void apply_hue_slider_style(lv_obj_t *slider)
{
    static lv_grad_dsc_t grad;
    static bool ready = false;
    if (!ready) {
        static const uint32_t cols[7] = {0xFF0000u, 0xFFA500u, 0xFFFF00u, 0x008000u,
                                         0x0000FFu, 0x800080u, 0xFF0000u};
        static const uint8_t fracs[7] = {0, 42, 85, 128, 170, 213, 255};
        grad.dir = LV_GRAD_DIR_HOR;
        grad.extend = LV_GRAD_EXTEND_PAD;
        grad.stops_count = 7;
        for (int i = 0; i < 7; i++) {
            grad.stops[i].color = lv_color_hex(cols[i]);
            grad.stops[i].opa = LV_OPA_COVER;
            grad.stops[i].frac = fracs[i];
        }
        ready = true;
    }
    lv_obj_set_height(slider, 16);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad(slider, &grad, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(UI_COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
}

static lv_color_t color_from_hs(uint16_t hue, uint8_t sat)
{
    uint8_t r, g, b;
    hsv_to_rgb8((float)(hue % 360u), (float)sat / 100.0f, 1.0f, &r, &g, &b);
    return color_from_rgb(r, g, b);
}

/* Превью и подписи по текущим положениям слайдеров оттенка/насыщенности. */
static void update_color_preview(ui_widget_t *widget)
{
    const uint16_t hue = (uint16_t)lv_slider_get_value(widget->hue_slider);
    const uint8_t sat = (uint8_t)lv_slider_get_value(widget->sat_slider);
    lv_obj_set_style_bg_color(widget->swatch, color_from_hs(hue, sat), 0);
    lv_obj_set_style_bg_color(widget->sat_slider, color_from_hs(hue, sat), LV_PART_KNOB);
    char text[16];
    snprintf(text, sizeof(text), "%u°", (unsigned)hue);
    lv_label_set_text(widget->hue_label, text);
    snprintf(text, sizeof(text), "%u %%", (unsigned)sat);
    lv_label_set_text(widget->sat_label, text);
}

static void on_color_changed(lv_event_t *event)
{
    ui_widget_t *widget = lv_event_get_user_data(event);
    update_color_preview(widget);
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) {
        return;
    }
    const uint16_t hue = (uint16_t)lv_slider_get_value(widget->hue_slider);
    const uint8_t sat = (uint8_t)lv_slider_get_value(widget->sat_slider);
    uint8_t r, g, b;
    hsv_to_rgb8((float)(hue % 360u), (float)sat / 100.0f, 1.0f, &r, &g, &b);
    uint16_t x, y;
    rgb8_to_xy(r, g, b, &x, &y);
    (void)display_send_color_xy(&widget->state, x, y);
}

#define UI_SWITCH_LOCK_MS 1500

static void switch_lock_cb(lv_timer_t *timer)
{
    ui_widget_t *widget = lv_timer_get_user_data(timer);
    if (widget != NULL && widget->obj != NULL) {
        lv_obj_remove_state(widget->obj, LV_STATE_DISABLED);
    }
    lv_timer_pause(timer);
}

/*
 * Нажатие меняет состояние по логике LVGL, команда уходит сразу, а сам свич блокируется
 * на UI_SWITCH_LOCK_MS, чтобы не нажать второй раз. Реальное состояние приходит только
 * репортом от Zigbee (ui_widget_apply) — респонзов мы не ждём (fire-and-forget).
 */
static void on_switch_changed(lv_event_t *event)
{
    ui_widget_t *widget = lv_event_get_user_data(event);
    const bool on = lv_obj_has_state(widget->obj, LV_STATE_CHECKED);
    (void)display_send_onoff(&widget->state, on);
    lv_obj_add_state(widget->obj, LV_STATE_DISABLED);
    if (widget->cooldown == NULL) {
        widget->cooldown = lv_timer_create(switch_lock_cb, UI_SWITCH_LOCK_MS, widget);
    }
    if (widget->cooldown != NULL) {
        lv_timer_set_period(widget->cooldown, UI_SWITCH_LOCK_MS);
        lv_timer_reset(widget->cooldown);
        lv_timer_resume(widget->cooldown);
    }
}

/*
 * Слайдеры — в семантических единицах (level/яркость: %, цветовая температура: K);
 * ZCL-аргументы команды конвертируются здесь (command-path — отдельная фаза).
 */
static void on_slider_released(lv_event_t *event)
{
    ui_widget_t *widget = lv_event_get_user_data(event);
    const int32_t value = lv_slider_get_value(widget->slider);
    if (widget->kind == UI_WIDGET_LEVEL) {
        const uint8_t level = (uint8_t)((value * 254 + 50) / 100);
        (void)display_send_level(&widget->state, level);
    } else if (widget->kind == UI_WIDGET_COLOR_TEMP) {
        const uint32_t kelvin = (uint32_t)value;
        const uint16_t mireds = kelvin > 0 ? (uint16_t)(1000000u / kelvin) : 0;
        (void)display_send_color_temperature(&widget->state, mireds);
    } else if (widget->kind == UI_WIDGET_COLOR) {
        const uint8_t level = (uint8_t)((value * 254 + 50) / 100); /* яркость, % */
        (void)display_send_level(&widget->state, level);
    }
}

/* Обёртка живёт ровно столько же, сколько её lv_obj: экран удаляется — виджет освобождён. */
static void on_widget_deleted(lv_event_t *event)
{
    ui_widget_t *widget = lv_event_get_user_data(event);
    if (widget != NULL) {
        if (widget->cooldown != NULL) {
            lv_timer_delete(widget->cooldown);
        }
        lv_free(widget);
    }
}

static void build_color_widget(lv_obj_t *parent, ui_widget_t *widget)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 12, 0);
    ui_set_scrollable(col, false);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Прямоугольник текущего цвета (X/Y вместе). */
    widget->swatch = lv_obj_create(col);
    lv_obj_set_size(widget->swatch, lv_pct(100), 64);
    lv_obj_set_style_radius(widget->swatch, 12, 0);
    lv_obj_set_style_border_width(widget->swatch, 2, 0);
    lv_obj_set_style_border_color(widget->swatch, lv_color_hex(UI_COL_MUTED), 0);
    lv_obj_set_style_bg_opa(widget->swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(widget->swatch, lv_color_hex(UI_COL_CHIP), 0);
    ui_set_scrollable(widget->swatch, false);

    make_slider_row(col, 0, 359, &widget->hue_slider, &widget->hue_label);
    apply_hue_slider_style(widget->hue_slider);
    lv_obj_add_event_cb(widget->hue_slider, on_color_changed, LV_EVENT_VALUE_CHANGED, widget);
    lv_obj_add_event_cb(widget->hue_slider, on_color_changed, LV_EVENT_RELEASED, widget);

    make_slider_row(col, 0, 100, &widget->sat_slider, &widget->sat_label);
    lv_obj_add_event_cb(widget->sat_slider, on_color_changed, LV_EVENT_VALUE_CHANGED, widget);
    lv_obj_add_event_cb(widget->sat_slider, on_color_changed, LV_EVENT_RELEASED, widget);

    /* Яркость (%). */
    make_slider_row(col, 0, 100, &widget->slider, &widget->value_label);
    lv_obj_add_event_cb(widget->slider, on_slider_released, LV_EVENT_RELEASED, widget);

    widget->obj = col;
}

ui_widget_t *ui_widget_create(domain_t *domain, lv_obj_t *parent,
                              const ha_zb_state_key_t *state)
{
    if (parent == NULL || state == NULL) {
        return NULL;
    }

    ui_widget_t *widget = lv_malloc(sizeof(*widget));
    if (widget == NULL) {
        return NULL;
    }
    *widget = (ui_widget_t){0};
    widget->domain = domain;
    widget->state = *state;
    widget->property = semantics_property_from_key(state);
    widget->kind = ui_widget_kind_for(widget->property);

    switch (widget->kind) {
    case UI_WIDGET_SWITCH:
        widget->obj = lv_switch_create(parent);
        lv_obj_set_size(widget->obj, 140, 70);
        lv_obj_add_event_cb(widget->obj, on_switch_changed, LV_EVENT_VALUE_CHANGED, widget);
        break;
    case UI_WIDGET_LEVEL:
        widget->obj = make_slider_row(parent, 0, 100, &widget->slider, &widget->value_label);
        lv_obj_add_event_cb(widget->slider, on_slider_released, LV_EVENT_RELEASED, widget);
        break;
    case UI_WIDGET_COLOR_TEMP:
        widget->obj = make_slider_row(parent, 2000, 6500, &widget->slider, &widget->value_label);
        lv_obj_add_event_cb(widget->slider, on_slider_released, LV_EVENT_RELEASED, widget);
        break;
    case UI_WIDGET_COLOR:
        build_color_widget(parent, widget);
        break;
    case UI_WIDGET_INDICATOR:
        widget->obj = make_indicator(parent, widget);
        break;
    case UI_WIDGET_VALUE:
    case UI_WIDGET_NONE:
    default:
        widget->kind = UI_WIDGET_VALUE;
        widget->obj = lv_label_create(parent);
        lv_obj_set_style_text_font(widget->obj, UI_FONT_VALUE, 0);
        lv_obj_set_style_text_color(widget->obj, lv_color_hex(UI_COL_TEXT), 0);
        lv_label_set_text(widget->obj, "—");
        widget->value_label = widget->obj;
        break;
    }

    lv_obj_add_event_cb(widget->obj, on_widget_deleted, LV_EVENT_DELETE, widget);
    return widget;
}

static void apply_absent(ui_widget_t *widget)
{
    switch (widget->kind) {
    case UI_WIDGET_SWITCH:
        if (!lv_obj_has_state(widget->obj, LV_STATE_DISABLED)) {
            lv_obj_remove_state(widget->obj, LV_STATE_CHECKED);
        }
        break;
    case UI_WIDGET_LEVEL:
    case UI_WIDGET_COLOR_TEMP:
        lv_slider_set_value(widget->slider, 0, LV_ANIM_OFF);
        lv_label_set_text(widget->value_label, "—");
        break;
    case UI_WIDGET_COLOR:
        lv_obj_set_style_bg_color(widget->swatch, lv_color_hex(UI_COL_CHIP), 0);
        lv_label_set_text(widget->hue_label, "—");
        lv_slider_set_value(widget->slider, 0, LV_ANIM_OFF);
        lv_label_set_text(widget->value_label, "—");
        break;
    case UI_WIDGET_INDICATOR:
        lv_obj_set_style_bg_color(widget->obj, lv_color_hex(UI_COL_CHIP), 0);
        lv_label_set_text(widget->value_label, "—");
        break;
    default:
        lv_label_set_text(widget->value_label, "—");
        break;
    }
}

/* Цвет и яркость соседних свойств — через обратный маппинг моста, без ручных ZCL-ключей. */
static void apply_color(ui_widget_t *widget)
{
    ha_zb_state_key_t xk, yk;
    ha_value_t xv, yv;
    const bool has_xy = semantics_property_key(&widget->state, HA_PROPERTY_COLOR_X, &xk) &&
                        semantics_property_key(&widget->state, HA_PROPERTY_COLOR_Y, &yk) &&
                        decode_state(widget->domain, &xk, &xv) &&
                        decode_state(widget->domain, &yk, &yv);

    char text[16];
    if (has_xy) {
        uint8_t r, g, b;
        const uint16_t x16 = (uint16_t)(clamp01f(xv.value.f32) * 65535.0f + 0.5f);
        const uint16_t y16 = (uint16_t)(clamp01f(yv.value.f32) * 65535.0f + 0.5f);
        xy_to_rgb8(x16, y16, &r, &g, &b);
        float h, s, v;
        rgb8_to_hsv(r, g, b, &h, &s, &v);
        const int sat = (int)(s * 100.0f + 0.5f);
        lv_obj_set_style_bg_color(widget->swatch, color_from_rgb(r, g, b), 0);
        lv_obj_set_style_bg_color(widget->sat_slider, color_from_rgb(r, g, b), LV_PART_KNOB);
        lv_slider_set_value(widget->hue_slider, (int32_t)h, LV_ANIM_OFF);
        lv_slider_set_value(widget->sat_slider, (int32_t)sat, LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%u°", (unsigned)(int)h);
        lv_label_set_text(widget->hue_label, text);
        snprintf(text, sizeof(text), "%d %%", sat);
        lv_label_set_text(widget->sat_label, text);
    } else {
        lv_obj_set_style_bg_color(widget->swatch, lv_color_hex(UI_COL_CHIP), 0);
        lv_label_set_text(widget->hue_label, "—");
        lv_label_set_text(widget->sat_label, "—");
    }

    ha_zb_state_key_t lk;
    ha_value_t lv;
    if (semantics_property_key(&widget->state, HA_PROPERTY_BRIGHTNESS, &lk) &&
        decode_state(widget->domain, &lk, &lv)) {
        const int pct = (int)(lv.value.f32 + 0.5f);
        lv_slider_set_value(widget->slider, pct, LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%d %%", pct);
        lv_label_set_text(widget->value_label, text);
    } else {
        lv_label_set_text(widget->value_label, "—");
    }
}

/* Управляющий элемент сейчас под пальцем — не перетираем его значение из Domain. */
static bool widget_control_pressed(const ui_widget_t *widget)
{
    const lv_obj_t *objs[4] = {widget->obj, widget->slider, widget->hue_slider, widget->sat_slider};
    for (size_t i = 0; i < 4; i++) {
        if (objs[i] != NULL && lv_obj_has_state((lv_obj_t *)objs[i], LV_STATE_PRESSED)) {
            return true;
        }
    }
    return false;
}

void ui_widget_apply(ui_widget_t *widget, const ha_zb_state_record_t *record, bool present)
{
    if (widget == NULL || widget->obj == NULL) {
        return;
    }
    /* Пока палец на слайдере/свиче — не применяем состояние из репорта, иначе
     * значение скачет между пальцем и последним отчётом. */
    if (widget_control_pressed(widget)) {
        return;
    }
    if (widget->kind == UI_WIDGET_COLOR) {
        apply_color(widget);
        return;
    }
    if (!present || record == NULL) {
        apply_absent(widget);
        return;
    }

    ha_value_t value = {0};
    if (!semantics_state_value(&widget->state, record, &value)) {
        apply_absent(widget);
        return;
    }

    char text[48] = {0};
    switch (widget->kind) {
    case UI_WIDGET_SWITCH: {
        const bool on = value.value.b;
        const bool fresh = !widget->has_last || (record->raw != widget->last_raw);
        if (lv_obj_has_state(widget->obj, LV_STATE_DISABLED)) {
            if (!fresh) {
                break; /* блок держим, пока не придёт новый репорт */
            }
            /* Пришло реальное состояние — резко снимаем блок и применяем его. */
            if (widget->cooldown != NULL) {
                lv_timer_pause(widget->cooldown);
            }
            lv_obj_remove_state(widget->obj, LV_STATE_DISABLED);
        }
        if (on) {
            lv_obj_add_state(widget->obj, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(widget->obj, LV_STATE_CHECKED);
        }
        widget->last_raw = record->raw;
        widget->has_last = true;
        break;
    }
    case UI_WIDGET_LEVEL: {
        const int pct = (int)(value.value.f32 + 0.5f);
        lv_slider_set_value(widget->slider, pct, LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%d %%", pct);
        lv_label_set_text(widget->value_label, text);
        break;
    }
    case UI_WIDGET_COLOR_TEMP: {
        const int kelvin = (int)(value.value.f32 + 0.5f);
        lv_slider_set_value(widget->slider, kelvin, LV_ANIM_OFF);
        snprintf(text, sizeof(text), "%d K", kelvin);
        lv_label_set_text(widget->value_label, text);
        break;
    }
    case UI_WIDGET_INDICATOR: {
        const bool active = value.value.b;
        const char *label = active ? "Занято" : "Свободно";
        const uint32_t color = active ? UI_COL_OK : UI_COL_CHIP;
        lv_obj_set_style_bg_color(widget->obj, lv_color_hex(color), 0);
        lv_label_set_text(widget->value_label, label);
        break;
    }
    case UI_WIDGET_VALUE:
    case UI_WIDGET_NONE:
    default:
        format_property(widget->property, &value, text, sizeof(text));
        lv_label_set_text(widget->value_label, text);
        break;
    }
}
