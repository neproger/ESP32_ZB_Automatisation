#include "ui_wifi.h"

#include <stdio.h>
#include <string.h>

#include "ha_model/ha_entities.h"
#include "ha_model/ha_wifi.h"
#include "ui_commands.h"
#include "ui_compat.h"
#include "ui_icons.h"
#include "ui_style.h"

#define UI_WIFI_HEADER_H 64

static domain_t *s_domain;
static ui_wifi_back_cb s_back;
static lv_obj_t *s_root;
static lv_obj_t *s_status;
static lv_obj_t *s_spinner;
static lv_obj_t *s_list;
static uint32_t s_scan_version;
static bool s_has_scan_version;

static lv_obj_t *s_backdrop;
static lv_obj_t *s_dialog;
static lv_obj_t *s_password;
static lv_obj_t *s_keyboard;
static char s_selected_ssid[HA_WIFI_SSID_MAX];
/* Копии ssid для обработчиков: указатель на запись скана (на стеке) жить не может. */
static char s_ssids[HA_WIFI_SCAN_MAX][HA_WIFI_SSID_MAX];

static void close_dialog(void);

static void on_back(lv_event_t *event)
{
    (void)event;
    if (s_back != NULL) {
        s_back();
    }
}

static void on_scan(lv_event_t *event)
{
    (void)event;
    (void)display_send_wifi_scan();
}

/* Экран удалён (lv_screen_load_anim с auto_del) — сбрасываем состояние и диалог. */
static void on_root_deleted(lv_event_t *event)
{
    if (lv_event_get_target(event) != s_root) {
        return; /* удаляется старый экран (anim auto_del) — текущие указатели не трогаем */
    }
    close_dialog();
    s_root = NULL;
    s_status = NULL;
    s_spinner = NULL;
    s_list = NULL;
    s_has_scan_version = false;
}

static void on_connect(lv_event_t *event)
{
    (void)event;
    const char *password = (s_password != NULL) ? lv_textarea_get_text(s_password) : "";
    (void)display_send_wifi_connect(s_selected_ssid, password);
    close_dialog();
}

static void on_cancel(lv_event_t *event)
{
    (void)event;
    close_dialog();
}

static void close_dialog(void)
{
    if (s_keyboard != NULL) {
        lv_obj_delete(s_keyboard);
        s_keyboard = NULL;
    }
    if (s_dialog != NULL) {
        lv_obj_delete(s_dialog);
        s_dialog = NULL;
    }
    if (s_backdrop != NULL) {
        lv_obj_delete(s_backdrop);
        s_backdrop = NULL;
    }
    s_password = NULL;
}

static void open_dialog(const char *ssid)
{
    close_dialog();
    snprintf(s_selected_ssid, sizeof(s_selected_ssid), "%s", ssid);

    lv_obj_t *layer = lv_layer_top();

    s_backdrop = lv_obj_create(layer);
    lv_obj_set_size(s_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_align(s_backdrop, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_backdrop, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_backdrop, 0, 0);
    lv_obj_set_style_pad_all(s_backdrop, 0, 0);
    ui_set_scrollable(s_backdrop, false);

    s_dialog = lv_obj_create(s_backdrop);
    lv_obj_set_size(s_dialog, 440, LV_SIZE_CONTENT);
    lv_obj_align(s_dialog, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + 20);
    lv_obj_set_style_bg_color(s_dialog, lv_color_hex(UI_COL_CARD), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dialog, 16, 0);
    lv_obj_set_style_border_width(s_dialog, 0, 0);
    lv_obj_set_style_pad_all(s_dialog, 16, 0);
    lv_obj_set_style_pad_row(s_dialog, 12, 0);
    ui_set_scrollable(s_dialog, false);
    lv_obj_set_flex_flow(s_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_dialog, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(s_dialog);
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text_fmt(title, "Пароль: %s", ssid);

    s_password = lv_textarea_create(s_dialog);
    lv_obj_set_width(s_password, lv_pct(100));
    lv_textarea_set_one_line(s_password, true);
    lv_textarea_set_password_mode(s_password, true);
    lv_obj_set_style_text_font(s_password, UI_FONT_BODY, 0);
    lv_textarea_set_placeholder_text(s_password, "Пароль");

    lv_obj_t *row = lv_obj_create(s_dialog);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    ui_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cancel = lv_button_create(row);
    lv_obj_set_size(cancel, 160, 52);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(UI_COL_CHIP), 0);
    lv_obj_add_event_cb(cancel, on_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "Отмена");
    lv_obj_set_style_text_font(cancel_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(cancel_label);

    lv_obj_t *connect = lv_button_create(row);
    lv_obj_set_size(connect, 160, 52);
    lv_obj_set_style_bg_color(connect, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_add_event_cb(connect, on_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect);
    lv_label_set_text(connect_label, "Подключить");
    lv_obj_set_style_text_font(connect_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(connect_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(connect_label);

    s_keyboard = lv_keyboard_create(layer);
    lv_obj_set_width(s_keyboard, lv_pct(100));
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, s_password);
}

static void on_ap_clicked(lv_event_t *event)
{
    const char *ssid = (const char *)lv_event_get_user_data(event);
    open_dialog(ssid);
}

static lv_obj_t *make_ap_row(lv_obj_t *list, const char *ssid, int8_t rssi, uint8_t auth)
{
    lv_obj_t *button = lv_button_create(list);
    lv_obj_set_width(button, lv_pct(100));
    lv_obj_set_height(button, 64);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COL_CARD), 0);
    lv_obj_set_style_pad_left(button, 16, 0);
    lv_obj_set_style_pad_right(button, 16, 0);
    /* ssid нужно пережить обработчик, поэтому копия в каждый ряд создаётся через
     * отдельный label + user_data. Держим ssid в статике ряда через текст label. */
    lv_obj_t *name = lv_label_create(button);
    lv_label_set_text_fmt(name, "%s%s", ssid, (auth != HA_WIFI_AUTH_OPEN) ? "  *" : "");
    lv_obj_set_style_text_font(name, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *signal = lv_label_create(button);
    lv_label_set_text_fmt(signal, "%d dBm", (int)rssi);
    lv_obj_set_style_text_font(signal, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(signal, lv_color_hex(UI_COL_MUTED), 0);
    lv_obj_align(signal, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_add_event_cb(button, on_ap_clicked, LV_EVENT_CLICKED, (void *)ssid);
    return button;
}

static void rebuild_list(void)
{
    if (s_list == NULL) {
        return;
    }
    lv_obj_clean(s_list);

    ha_wifi_scan_record_t scan = {0};
    const ha_device_uid_t uid = HA_WIFI_DEVICE_UID;
    if (!sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_WIFI_SCAN, &uid, &scan)) ||
        scan.count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "Нет сетей — нажмите «Сканировать»");
        lv_obj_set_style_text_font(empty, UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(UI_COL_MUTED), 0);
        return;
    }
    const uint8_t count = (scan.count > HA_WIFI_SCAN_MAX) ? HA_WIFI_SCAN_MAX : scan.count;
    for (uint8_t i = 0; i < count; ++i) {
        snprintf(s_ssids[i], sizeof(s_ssids[i]), "%s", scan.aps[i].ssid);
        make_ap_row(s_list, s_ssids[i], scan.aps[i].rssi, scan.aps[i].auth);
    }
}

static void apply_status(void)
{
    ha_wifi_status_record_t status = {0};
    const ha_device_uid_t uid = HA_WIFI_DEVICE_UID;
    const bool present =
        sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_WIFI_STATUS, &uid, &status));

    const bool busy = present && (status.state == HA_WIFI_STATE_SCANNING ||
                                  status.state == HA_WIFI_STATE_CONNECTING);
    ui_set_hidden(s_spinner, !busy);

    char text[96] = "Не подключено";
    if (present) {
        switch (status.state) {
        case HA_WIFI_STATE_SCANNING:
            snprintf(text, sizeof(text), "Поиск сетей…");
            break;
        case HA_WIFI_STATE_CONNECTING:
            snprintf(text, sizeof(text), "Подключение…");
            break;
        case HA_WIFI_STATE_CONNECTED:
            snprintf(text, sizeof(text), "Подключено: %s (%d dBm)", status.ssid, (int)status.rssi);
            break;
        case HA_WIFI_STATE_ERROR:
            snprintf(text, sizeof(text), "Ошибка подключения");
            break;
        default:
            break;
        }
    }
    lv_label_set_text(s_status, text);
}

static void apply_scan(void)
{
    domain_entity_meta_t meta = {0};
    const ha_device_uid_t uid = HA_WIFI_DEVICE_UID;
    if (!sys_ok(domain_entity_meta(s_domain, (domain_entity_t)HA_ENTITY_WIFI_SCAN, &uid, &meta))) {
        if (s_has_scan_version) {
            s_has_scan_version = false;
            rebuild_list();
        }
        return;
    }
    if (!s_has_scan_version || meta.version.value != s_scan_version) {
        s_scan_version = meta.version.value;
        s_has_scan_version = true;
        rebuild_list();
    }
}

lv_obj_t *ui_wifi_create(domain_t *domain, ui_wifi_back_cb back)
{
    s_domain = domain;
    s_back = back;

    s_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    ui_set_scrollable(s_root, false);
    lv_obj_add_event_cb(s_root, on_root_deleted, LV_EVENT_DELETE, NULL);

    /* Шапка: назад + заголовок. */
    lv_obj_t *back_btn = lv_button_create(s_root);
    lv_obj_set_size(back_btn, 56, 44);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 12, UI_STATUSBAR_H + 10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(UI_COL_CHIP), 0);
    lv_obj_add_event_cb(back_btn, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, UI_ICON_ANGLE_LEFT);
    lv_obj_set_style_text_font(back_label, &ui_icons, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + 18);

    /* Статус + спиннер. */
    s_spinner = lv_spinner_create(s_root);
    lv_obj_set_size(s_spinner, 28, 28);
    lv_obj_align(s_spinner, LV_ALIGN_TOP_LEFT, 16, UI_STATUSBAR_H + UI_WIFI_HEADER_H + 14);
    lv_obj_set_style_arc_width(s_spinner, 4, LV_PART_INDICATOR);
    ui_set_hidden(s_spinner, true);

    s_status = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_status, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COL_MUTED), 0);
    lv_label_set_text(s_status, "Не подключено");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + UI_WIFI_HEADER_H + 14);

    /* Список сетей. */
    s_list = lv_obj_create(s_root);
    lv_obj_set_size(s_list, 456, 480);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + UI_WIFI_HEADER_H + 52);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Кнопка сканирования. */
    lv_obj_t *scan = lv_button_create(s_root);
    lv_obj_set_size(scan, 240, 56);
    lv_obj_align(scan, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(scan, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_add_event_cb(scan, on_scan, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_label = lv_label_create(scan);
    lv_label_set_text(scan_label, "Сканировать");
    lv_obj_set_style_text_font(scan_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(scan_label, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_center(scan_label);

    s_has_scan_version = false;
    return s_root;
}

void ui_wifi_apply(void)
{
    if (s_root == NULL) {
        return;
    }
    apply_status();
    apply_scan();
}
