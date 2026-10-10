#include "display/display.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_settings.h"
#include "ha_model/ha_weather.h"
#include "ha_model/ha_wifi.h"
#include "ha_model/ha_zigbee.h"
#include "semantics/semantics.h"
#include "ui_commands.h"
#include "ui_menu.h"
#include "ui_nav_dots.h"
#include "ui_page.h"
#include "ui_palette.h"
#include "ui_settings.h"
#include "ui_status_bar.h"
#include "ui_style.h"
#include "ui_wifi.h"

#define DISPLAY_POLL_PERIOD_MS 250
#define DISPLAY_ANIM_MS 220
#define UI_MAX_GROUPS 24
#define UI_MAX_ITEMS 128
#define UI_ITEM_TITLE_MAX 64

typedef struct {
    size_t count;
    ha_group_key_t ids[UI_MAX_GROUPS];
} group_list_t;

typedef struct {
    size_t count;
    ha_group_item_key_t keys[UI_MAX_ITEMS];
    ha_group_item_record_t recs[UI_MAX_ITEMS];
} item_list_t;

typedef enum {
    SCREEN_GROUPS = 0,
    SCREEN_WIFI,
    SCREEN_SETTINGS,
} screen_mode_t;

static domain_t *s_domain;
static ui_page_t *s_page;
static screen_mode_t s_screen_mode;
static size_t s_group_index;
static group_list_t s_groups;
static item_list_t s_items;

/* Буферы под дескрипторы виджетов экрана: строятся в build_page и скопированы
 * ui_page_create (labels + entries). Живут статически — экран один, вызов один. */
static ui_page_item_t s_descs[UI_MAX_ITEMS];
static char s_titles[UI_MAX_ITEMS][UI_ITEM_TITLE_MAX];

/* --- сбор данных --- */

static bool group_cb(const void *key, const void *record, void *ctx)
{
    (void)record;
    group_list_t *list = (group_list_t *)ctx;
    if (list->count < UI_MAX_GROUPS) {
        list->ids[list->count] = *(const ha_group_key_t *)key;
        list->count++;
    }
    return true;
}

typedef struct {
    uint64_t group_id;
    item_list_t *list;
} item_filter_t;

static bool item_cb(const void *key, const void *record, void *ctx)
{
    const ha_group_item_key_t *item_key = (const ha_group_item_key_t *)key;
    item_filter_t *filter = (item_filter_t *)ctx;
    if (item_key->group_id != filter->group_id) {
        return true;
    }
    if (filter->list->count < UI_MAX_ITEMS) {
        const size_t at = filter->list->count;
        filter->list->keys[at] = *item_key;
        filter->list->recs[at] = *(const ha_group_item_record_t *)record;
        filter->list->count++;
    }
    return true;
}

static void collect_groups(void)
{
    s_groups.count = 0;
    (void)domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_GROUP, group_cb, &s_groups);
}

static void sort_items(item_list_t *list)
{
    for (size_t i = 1; i < list->count; ++i) {
        const ha_group_item_key_t key = list->keys[i];
        const ha_group_item_record_t record = list->recs[i];
        size_t j = i;
        while (j > 0 && list->recs[j - 1].order > record.order) {
            list->keys[j] = list->keys[j - 1];
            list->recs[j] = list->recs[j - 1];
            --j;
        }
        list->keys[j] = key;
        list->recs[j] = record;
    }
}

static void collect_items(size_t group_index)
{
    s_items.count = 0;
    if (group_index >= s_groups.count) {
        return;
    }
    item_filter_t filter = {
        .group_id = s_groups.ids[group_index].id,
        .list = &s_items,
    };
    (void)domain_entity_iter(s_domain, (domain_entity_t)HA_ENTITY_GROUP_ITEM, item_cb, &filter);
    sort_items(&s_items);
}

static bool read_group(size_t index, ha_group_record_t *out)
{
    return (index < s_groups.count) &&
           sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_GROUP,
                                    &s_groups.ids[index], out));
}

static void item_title(size_t index, char *out, size_t out_size)
{
    snprintf(out, out_size, "—");
    if (index >= s_items.count) {
        return;
    }
    const ha_group_item_record_t *record = &s_items.recs[index];
    if (record->title[0] != '\0') {
        snprintf(out, out_size, "%s", record->title);
        return;
    }
    const ha_zb_state_key_t *state = &s_items.keys[index].state;
    ha_device_record_t device = {0};
    const char *name = NULL;
    if (sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_DEVICE, &state->device_uid,
                                 &device))) {
        if (device.name[0] != '\0') {
            name = device.name; /* имя, заданное пользователем из UI */
        } else if (device.model[0] != '\0') {
            name = device.model;
        }
    }
    if (name != NULL) {
        snprintf(out, out_size, "%s · EP%u", name, (unsigned)state->endpoint);
    } else {
        snprintf(out, out_size, "%016llX · EP%u", (unsigned long long)state->device_uid,
                 (unsigned)state->endpoint);
    }
}

/* --- экраны --- */

static void on_page_gesture(void *ctx, lv_dir_t dir);

static ui_page_t *build_page(size_t group_index)
{
    char group_title[HA_GROUP_TITLE_MAX] = "Нет экранов";
    size_t count = 0;

    if (group_index < s_groups.count) {
        ha_group_record_t group = {0};
        if (read_group(group_index, &group) && group.title[0] != '\0') {
            snprintf(group_title, sizeof(group_title), "%s", group.title);
        } else {
            snprintf(group_title, sizeof(group_title), "Экран");
        }
        collect_items(group_index);
        count = s_items.count;
        for (size_t i = 0; i < count; ++i) {
            item_title(i, s_titles[i], sizeof(s_titles[i]));
            s_descs[i].title = s_titles[i];
            s_descs[i].state = &s_items.keys[i].state;
        }
    }
    return ui_page_create(s_domain, group_title, s_descs, count, on_page_gesture, NULL);
}

/* Экран без анимации: старт и восстановление при рассинхроне (группу удалили). */
static void load_page_now(void)
{
    ui_page_t *next = build_page(s_group_index);
    if (next == NULL) {
        return;
    }
    ui_page_t *old = s_page;
    s_page = next;
    lv_screen_load(ui_page_root(next));
    if (old != NULL) {
        lv_obj_t *root = ui_page_root(old);
        if (root != NULL) {
            lv_obj_delete(root);
        }
    }
}

static void navigate(int group_delta)
{
    if (s_domain == NULL || s_screen_mode != SCREEN_GROUPS) {
        return;
    }
    collect_groups();
    if (s_groups.count == 0) {
        if (s_page == NULL) {
            load_page_now();
        }
        return;
    }
    if (s_group_index >= s_groups.count) {
        s_group_index = s_groups.count - 1;
    }

    const int count = (int)s_groups.count;
    int next = ((int)s_group_index + group_delta) % count;
    if (next < 0) {
        next += count;
    }
    s_group_index = (size_t)next;
    ui_nav_dots_update(s_groups.count, s_group_index);

    ui_page_t *page = build_page(s_group_index);
    if (page == NULL) {
        return;
    }
    s_page = page;
    lv_screen_load_anim(ui_page_root(page),
                        (group_delta > 0) ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT
                                          : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
                        DISPLAY_ANIM_MS, 0, true);
}

static void step_group(int delta)
{
    navigate(delta);
}

static void on_page_gesture(void *ctx, lv_dir_t dir)
{
    (void)ctx;
    if (dir == LV_DIR_LEFT) {
        step_group(1);
    } else if (dir == LV_DIR_RIGHT) {
        step_group(-1);
    }
}

/* --- меню, экраны Wi-Fi и настроек --- */

/* Возврат из вспомогательного экрана к экранам групп (общий back для них). */
static void return_to_groups(void)
{
    s_screen_mode = SCREEN_GROUPS;
    collect_groups();
    s_page = NULL; /* текущий вспомогательный экран удалит lv_screen_load_anim(auto_del) */
    ui_page_t *page = build_page(s_group_index);
    if (page == NULL) {
        return;
    }
    s_page = page;
    lv_screen_load_anim(ui_page_root(page), LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, DISPLAY_ANIM_MS, 0, true);
    ui_nav_dots_update(s_groups.count, s_group_index);
}

static void open_wifi(void)
{
    collect_groups();
    s_page = NULL; /* текущий групповой экран удалит lv_screen_load_anim(auto_del) */
    lv_obj_t *root = ui_wifi_create(s_domain, return_to_groups);
    if (root == NULL) {
        return;
    }
    s_screen_mode = SCREEN_WIFI;
    lv_screen_load_anim(root, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, DISPLAY_ANIM_MS, 0, true);
    ui_nav_dots_update(1, 0); /* одна «страница» — точки скрыты */
}

static void open_settings(void)
{
    collect_groups();
    s_page = NULL;
    lv_obj_t *root = ui_settings_create(s_domain, return_to_groups, display_request_theme_reload);
    if (root == NULL) {
        return;
    }
    s_screen_mode = SCREEN_SETTINGS;
    lv_screen_load_anim(root, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, DISPLAY_ANIM_MS, 0, true);
    ui_nav_dots_update(1, 0);
}

static void on_menu(ui_menu_item_t item)
{
    if (item == UI_MENU_WIFI) {
        open_wifi();
    } else if (item == UI_MENU_SETTINGS) {
        open_settings();
    }
}

/* --- публичный API --- */

static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    display_poll();
}

/*
 * Дефолтный шрифт темы — наш кириллический. Иначе любой виджет без явного шрифта
 * (кнопки, список дропдауна, клавиатура) берёт встроенный ASCII-шрифт LVGL и
 * рисует кириллицу прямоугольниками.
 */
static void apply_theme_font(void)
{
    lv_display_t *display = lv_display_get_default();
    if (display == NULL) {
        return;
    }
    lv_theme_t *theme = lv_theme_default_init(display, lv_color_hex(UI_COL_ACCENT),
                                              lv_color_hex(UI_COL_OK), true, UI_FONT_BODY);
    lv_display_set_theme(display, theme);
}

/* Тема хранится в settings (пишет экран настроек); по умолчанию — Retro. */
static ui_palette_id_t read_settings_palette(void)
{
    const ha_settings_key_t key = {.id = HA_SETTINGS_ID};
    ha_settings_record_t settings = {0};
    if (sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_SETTINGS, &key, &settings))) {
        return (ui_palette_id_t)settings.palette_id;
    }
    return UI_PALETTE_RETRO;
}

/* Верхний слой (строка состояния, точки, меню) — общий для старта и смены темы. */
static void create_chrome(void)
{
    apply_theme_font();
    ui_status_bar_create(s_domain);
    ui_nav_dots_create();
    ui_menu_create();
    ui_menu_set_cb(on_menu);
}

static void build_ui(void)
{
    create_chrome();
    collect_groups();
    s_group_index = 0;
    load_page_now();
}

/*
 * Смена темы: LVGL фиксирует цвет при создании объекта, поэтому пересобираем верхний
 * слой и текущий экран. Вызывается из настроек через async, чтобы не удалять экран
 * внутри его же обработчика события.
 */
static void reload_ui_async(void *unused)
{
    (void)unused;
    ui_palette_set(read_settings_palette());
    lv_obj_clean(lv_layer_top());
    create_chrome();
    collect_groups();
    if (s_screen_mode == SCREEN_SETTINGS) {
        open_settings();
    } else if (s_screen_mode == SCREEN_WIFI) {
        open_wifi();
    } else {
        load_page_now();
    }

    /* После пересборки прогнать один цикл: заполнить короткие метки (строка состояния,
     * виджеты экрана) актуальными данными из Domain — без «костылей на каждый виджет». */
    display_poll();
}

void display_request_theme_reload(void)
{
    (void)lv_async_call(reload_ui_async, NULL);
}

void display_start(domain_t *domain)
{
    if (domain == NULL) {
        return;
    }
    s_domain = domain;
    ui_palette_set(read_settings_palette());
    build_ui();

    (void)lv_timer_create(poll_timer_cb, DISPLAY_POLL_PERIOD_MS, NULL);
}

void display_poll(void)
{
    if (s_domain == NULL) {
        return;
    }
    ui_status_bar_apply();
    if (s_screen_mode == SCREEN_WIFI) {
        ui_wifi_apply();
        return;
    }
    if (s_screen_mode == SCREEN_SETTINGS) {
        ui_settings_apply();
        return;
    }
    if (s_page == NULL) {
        return;
    }
    collect_groups();
    if (s_groups.count > 0 && s_group_index >= s_groups.count) {
        s_group_index = s_groups.count - 1;
        load_page_now();
    } else {
        ui_page_apply(s_page);
    }
    ui_nav_dots_update(s_groups.count, s_group_index);
}

/* Семантическая команда: ZCL-кодировку (scale/transition/direction/LE/args_len) делает мост. */
bool display_send_command(const ha_zb_state_key_t *target, ha_property_id_t property,
                          ha_action_id_t action, const ha_command_value_t *value)
{
    if (s_domain == NULL || target == NULL) {
        return false;
    }
    ha_zb_command_t command = {0};
    if (!semantics_build_command(target, property, action, value, &command)) {
        return false;
    }

    const domain_fact_target_t fact = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    return sys_ok(domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &fact,
                              NULL));
}

bool display_send_wifi_scan(void)
{
    if (s_domain == NULL) {
        return false;
    }
    return sys_ok(domain_post(s_domain, HA_CMD_WIFI_SCAN, NULL, 0, NULL, NULL));
}

bool display_send_wifi_connect(const char *ssid, const char *password)
{
    if (s_domain == NULL || ssid == NULL) {
        return false;
    }
    ha_wifi_connect_args_t args = {0};
    snprintf(args.ssid, sizeof(args.ssid), "%s", ssid);
    if (password != NULL) {
        snprintf(args.password, sizeof(args.password), "%s", password);
    }
    return sys_ok(domain_post(s_domain, HA_CMD_WIFI_CONNECT, &args, sizeof(args), NULL, NULL));
}
