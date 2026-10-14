#include "display/display.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "domain/domain.h"
#include "domain/domain_event.h"
#include "lvgl.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_entity.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_state.h"
#include "ha_model/ha_settings.h"
#include "ha_model/ha_weather.h"
#include "ha_model/ha_wifi.h"
#include "ui_commands.h"
#include "ui_menu.h"
#include "ui_nav_dots.h"
#include "ui_page.h"
#include "ui_palette.h"
#include "ui_settings.h"
#include "ui_status_bar.h"
#include "ui_style.h"
#include "ui_wifi.h"

#define DISPLAY_DRAIN_PERIOD_MS 50
#define DISPLAY_ANIM_MS 220
#define DISPLAY_INBOX_LEN 32
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

/* Подписка Domain: факты приходят в inbox, обрабатываем в задаче LVGL (drain). */
static QueueHandle_t s_inbox;
static bool s_screen_dirty;

/* Привод подсветки, регистрируемый портом; -1 = ещё не применено. */
static display_brightness_fn s_brightness_fn;
static int s_brightness_applied = -1;

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
    /* Заголовок плитки — имя Entity (A4: identity — entity_id, не устройство). */
    const ha_entity_id_t entity = s_items.keys[index].entity;
    const ha_entity_key_t key = {.id = entity};
    ha_entity_record_t record_entity = {0};
    if (sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_ENTITY, &key,
                                 &record_entity)) &&
        record_entity.name[0] != '\0') {
        snprintf(out, out_size, "%s", record_entity.name);
    } else {
        snprintf(out, out_size, "%016llX", (unsigned long long)entity);
    }
}

/* --- экраны --- */

static void on_page_gesture(void *ctx, lv_dir_t dir);

static ui_page_t *build_page(size_t group_index)
{
    char group_title[HA_GROUP_TITLE_MAX] = "Нет экранов";

    if (group_index < s_groups.count) {
        ha_group_record_t group = {0};
        if (read_group(group_index, &group) && group.title[0] != '\0') {
            snprintf(group_title, sizeof(group_title), "%s", group.title);
        } else {
            snprintf(group_title, sizeof(group_title), "Экран");
        }
    }
    collect_items(group_index); /* невалидный индекс → пустой список */
    const size_t count = s_items.count;
    for (size_t i = 0; i < count; ++i) {
        item_title(i, s_titles[i], sizeof(s_titles[i]));
        s_descs[i].title = s_titles[i];
        s_descs[i].entity = s_items.keys[i].entity;
    }
    return ui_page_create(s_domain, group_title, s_descs, count, on_page_gesture, NULL);
}

/* Экран без анимации: старт, восстановление при рассинхроне и пересборка по фактам. */
static void load_page_now(void)
{
    collect_groups();
    if (s_groups.count > 0 && s_group_index >= s_groups.count) {
        s_group_index = s_groups.count - 1;
    }
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
    ui_page_apply(s_page); /* разовая подсевка значений после сборки */
    ui_nav_dots_update(s_groups.count, (s_groups.count > 0) ? s_group_index : 0);
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
    ui_page_apply(s_page); /* подсевка значений нового экрана */
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
    ui_page_apply(s_page);
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

static void drain_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    display_process_events();
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

void display_register_brightness(display_brightness_fn fn)
{
    s_brightness_fn = fn;
}

void display_set_brightness(uint8_t percent)
{
    if (s_brightness_fn != NULL) {
        s_brightness_fn(percent);
    }
}

/* Подсветка из settings (пишет экран настроек или веб). Применяем только на изменение.
 * 0 в записи — «не задано» (легаси-дефолт) → полная яркость; слайдер ограничен снизу. */
static void apply_settings_brightness(void)
{
    const ha_settings_key_t key = {.id = HA_SETTINGS_ID};
    ha_settings_record_t settings = {0};
    int target =
        sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_SETTINGS, &key, &settings))
            ? (int)settings.brightness_pct
            : 100;
    if (target <= 0) {
        target = 100;
    }
    if (target != s_brightness_applied) {
        display_set_brightness((uint8_t)target);
        s_brightness_applied = target;
    }
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
    ui_status_bar_apply();
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
    display_process_events();
}

void display_request_theme_reload(void)
{
    (void)lv_async_call(reload_ui_async, NULL);
}

static void display_subscribe(domain_t *domain);

void display_start(domain_t *domain)
{
    if (domain == NULL) {
        return;
    }
    s_domain = domain;
    ui_palette_set(read_settings_palette());
    apply_settings_brightness();
    display_subscribe(domain); /* до build_ui: не теряем факты во время сборки */
    build_ui();

    (void)lv_timer_create(drain_timer_cb, DISPLAY_DRAIN_PERIOD_MS, NULL);
}

/* --- подписка Domain ---------------------------------------------------- */

static bool display_accept(const domain_event_t *event, void *ctx)
{
    (void)ctx;
    if (s_inbox == NULL) {
        return false;
    }
    return xQueueSend(s_inbox, event, 0) == pdTRUE;
}

static void display_subscribe(domain_t *domain)
{
    s_inbox = xQueueCreate(DISPLAY_INBOX_LEN, sizeof(domain_event_t));
    if (s_inbox == NULL) {
        return;
    }
    domain_subscription_desc_t desc = {0};
    desc.kind_mask = (1u << (uint32_t)DOMAIN_FACT_ENTITY_UPSERTED) |
                     (1u << (uint32_t)DOMAIN_FACT_ENTITY_REMOVED) |
                     (1u << (uint32_t)DOMAIN_FACT_EVENT);
    desc.source_mask = 0; /* любые источники */
    desc.entity = 0;      /* любые типы */
    desc.try_push = display_accept;
    desc.ctx = NULL;
    domain_subscription_t *sub = NULL;
    (void)domain_subscribe(domain, &desc, &sub);
}

/* Один факт: обновляем то, что от него зависит (без чтений «на всякий случай»). */
static void process_fact(const domain_event_t *event)
{
    const bool upserted = (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_UPSERTED);
    const bool removed = (event->kind == (uint8_t)DOMAIN_FACT_ENTITY_REMOVED);
    if (!upserted && !removed) {
        return; /* Display рендерит изменение сущностей, не события */
    }
    switch ((ha_entity_t)event->entity) {
    case HA_ENTITY_GROUP:
    case HA_ENTITY_GROUP_ITEM:
    case HA_ENTITY_ENTITY:
    case HA_ENTITY_CAPABILITIES:
        s_screen_dirty = true; /* состав/имя/возможности → пересобрать текущий экран */
        break;
    case HA_ENTITY_PROPERTY_STATE: {
        ha_state_key_t key = {0};
        if (event->key_size == sizeof(key)) {
            memcpy(&key, event->key, sizeof(key));
        }
        if (s_screen_mode == SCREEN_GROUPS && s_page != NULL) {
            ha_state_record_t record = {0};
            const bool present =
                upserted && sys_ok(domain_entity_get(
                                s_domain, (domain_entity_t)HA_ENTITY_PROPERTY_STATE, &key, &record));
            ui_page_apply_key(s_page, &key, &record.value, present);
        }
        if (key.entity == ha_entity_singleton(HA_ENTITY_ISSUER_SYSTEM)) {
            ui_status_bar_apply(); /* время системной Entity */
        }
        break;
    }
    case HA_ENTITY_WEATHER:
    case HA_ENTITY_LOCATION:
        ui_status_bar_apply();
        break;
    case HA_ENTITY_SETTINGS:
        apply_settings_brightness();
        if (s_screen_mode == SCREEN_SETTINGS) {
            ui_settings_apply();
        }
        break;
    case HA_ENTITY_WIFI_SCAN:
    case HA_ENTITY_WIFI_STATUS:
        if (s_screen_mode == SCREEN_WIFI) {
            ui_wifi_apply();
        }
        break;
    default:
        break;
    }
}

/* Дренаж inbox: вызывается из задачи LVGL. Domain читаем только по пришедшим фактам. */
void display_process_events(void)
{
    if (s_domain == NULL || s_inbox == NULL) {
        return;
    }
    domain_event_t event = {0};
    while (xQueueReceive(s_inbox, &event, 0) == pdTRUE) {
        process_fact(&event);
    }
    if (s_screen_dirty && s_screen_mode == SCREEN_GROUPS) {
        s_screen_dirty = false;
        load_page_now();
    }
}

/* Семантическая команда по entity: adapter резолвит entity→addr и кодирует (HA_CMD_PROPERTY). */
bool display_send_command(ha_entity_id_t entity, ha_property_id_t property, ha_action_id_t action,
                          const ha_command_value_t *value)
{
    if (s_domain == NULL) {
        return false;
    }
    ha_property_command_t command = {0};
    command.entity = entity;
    command.property = property;
    command.action = (uint8_t)action;
    if (value != NULL) {
        command.value = *value;
    }
    return sys_ok(domain_post(s_domain, HA_CMD_PROPERTY, &command, sizeof(command), NULL, NULL));
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
