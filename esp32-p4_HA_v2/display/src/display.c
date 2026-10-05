#include "display/display.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_zigbee.h"
#include "ui_commands.h"
#include "ui_nav_dots.h"
#include "ui_page.h"
#include "ui_status_bar.h"
#include "ui_style.h"

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

static domain_t *s_domain;
static ui_page_t *s_page;
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
    const uint64_t uid = s_items.keys[index].state.device_uid;
    ha_device_record_t device = {0};
    if (sys_ok(domain_entity_get(s_domain, (domain_entity_t)HA_ENTITY_DEVICE, &uid, &device)) &&
        device.name[0] != '\0') {
        snprintf(out, out_size, "%s", device.name);
        return;
    }
    snprintf(out, out_size, "%016llX", (unsigned long long)uid);
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
    if (s_domain == NULL) {
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

/* --- публичный API --- */

static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    display_poll();
}

void display_start(domain_t *domain)
{
    if (domain == NULL) {
        return;
    }
    s_domain = domain;

    ui_status_bar_create(domain);
    ui_nav_dots_create();
    collect_groups();
    s_group_index = 0;
    load_page_now();

    (void)lv_timer_create(poll_timer_cb, DISPLAY_POLL_PERIOD_MS, NULL);
}

void display_poll(void)
{
    if (s_domain == NULL || s_page == NULL) {
        return;
    }
    ui_status_bar_apply();
    collect_groups();
    if (s_groups.count > 0 && s_group_index >= s_groups.count) {
        s_group_index = s_groups.count - 1;
        load_page_now();
    } else {
        ui_page_apply(s_page);
    }
    ui_nav_dots_update(s_groups.count, s_group_index);
}

bool display_send_onoff(const ha_zb_state_key_t *key, bool on)
{
    if (s_domain == NULL || key == NULL) {
        return false;
    }
    ha_zb_command_t command = {0};
    command.device_uid = key->device_uid;
    command.dst_endpoint = key->endpoint;
    command.cluster_id = HA_ZB_CLUSTER_ON_OFF;
    command.command_id = on ? HA_ZB_CMD_ON_OFF_ON : HA_ZB_CMD_ON_OFF_OFF;

    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    return sys_ok(domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &target,
                              NULL));
}

bool display_send_level(const ha_zb_state_key_t *key, uint8_t level)
{
    if (s_domain == NULL || key == NULL) {
        return false;
    }
    ha_zb_command_t command = {0};
    command.device_uid = key->device_uid;
    command.dst_endpoint = key->endpoint;
    command.cluster_id = HA_ZB_CLUSTER_LEVEL_CONTROL;
    command.command_id = HA_ZB_CMD_LEVEL_MOVE_TO_LEVEL;
    command.args_len = 1;
    command.args[0] = level;

    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    return sys_ok(domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &target,
                              NULL));
}

bool display_send_hue(const ha_zb_state_key_t *key, uint8_t hue)
{
    if (s_domain == NULL || key == NULL) {
        return false;
    }
    ha_zb_command_t command = {0};
    command.device_uid = key->device_uid;
    command.dst_endpoint = key->endpoint;
    command.cluster_id = HA_ZB_CLUSTER_COLOR_CONTROL;
    command.command_id = HA_ZB_CMD_COLOR_MOVE_TO_HUE;
    command.args_len = 4;
    command.args[0] = hue;    /* hue 0..254 */
    command.args[1] = 0;      /* direction: shortest */
    command.args[2] = 0;      /* transition time, LE */
    command.args[3] = 0;

    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    return sys_ok(domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &target,
                              NULL));
}

bool display_send_saturation(const ha_zb_state_key_t *key, uint8_t saturation)
{
    if (s_domain == NULL || key == NULL) {
        return false;
    }
    ha_zb_command_t command = {0};
    command.device_uid = key->device_uid;
    command.dst_endpoint = key->endpoint;
    command.cluster_id = HA_ZB_CLUSTER_COLOR_CONTROL;
    command.command_id = HA_ZB_CMD_COLOR_MOVE_TO_SATURATION;
    command.args_len = 3;
    command.args[0] = saturation; /* 0..254 */
    command.args[1] = 0;          /* transition time, LE */
    command.args[2] = 0;

    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    return sys_ok(domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &target,
                              NULL));
}

bool display_send_color_temperature(const ha_zb_state_key_t *key, uint16_t mireds)
{
    if (s_domain == NULL || key == NULL) {
        return false;
    }
    ha_zb_command_t command = {0};
    command.device_uid = key->device_uid;
    command.dst_endpoint = key->endpoint;
    command.cluster_id = HA_ZB_CLUSTER_COLOR_CONTROL;
    command.command_id = HA_ZB_CMD_COLOR_MOVE_TO_COLOR_TEMPERATURE;
    command.args_len = 4;
    command.args[0] = (uint8_t)(mireds & 0xFFu);
    command.args[1] = (uint8_t)(mireds >> 8);
    command.args[2] = 0; /* transition time, LE */
    command.args[3] = 0;

    const domain_fact_target_t target = {
        .entity = (domain_entity_t)HA_ENTITY_DEVICE,
        .key = &command.device_uid,
    };
    return sys_ok(domain_post(s_domain, HA_CMD_ZIGBEE_CLUSTER, &command, sizeof(command), &target,
                              NULL));
}
