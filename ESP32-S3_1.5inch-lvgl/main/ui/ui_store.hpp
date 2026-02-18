#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gw_core/device_registry.h"
#include "gw_core/event_bus.h"
#include "gw_core/group_store.h"
#include "gw_core/state_store.h"
#include "gw_core/zb_model.h"
#include "ui_mapper.hpp"

static constexpr size_t UI_STORE_DEVICE_CAP = 64;
static constexpr size_t UI_STORE_ENDPOINT_CAP = 8;
static constexpr size_t UI_STORE_GROUP_CAP = 24;
static constexpr size_t UI_STORE_GROUP_ITEM_CAP = 32;

typedef struct
{
    uint8_t endpoint_id;
    char kind[24];
    ui_endpoint_caps_t caps;

    bool has_onoff;
    bool onoff;

    bool has_level;
    uint16_t level;

    bool has_temperature_c;
    float temperature_c;

    bool has_humidity_pct;
    float humidity_pct;

    bool has_battery_pct;
    uint32_t battery_pct;

    bool has_color_x;
    uint16_t color_x;
    bool has_color_y;
    uint16_t color_y;
} ui_endpoint_vm_t;

typedef struct
{
    gw_device_uid_t uid;
    uint16_t short_addr;
    char device_name[32];
    char label[32];
    uint8_t endpoint_id;
    uint32_t order;
    ui_endpoint_vm_t endpoint;
} ui_group_item_vm_t;

typedef struct
{
    char id[GW_GROUP_ID_MAX];
    char name[GW_GROUP_NAME_MAX];
    size_t active_item_idx;
    size_t item_count;
    ui_group_item_vm_t items[UI_STORE_GROUP_ITEM_CAP];
} ui_group_vm_t;

typedef struct
{
    size_t group_count;
    size_t active_group_idx;
    ui_group_vm_t groups[UI_STORE_GROUP_CAP];
} ui_store_t;

void ui_store_init(ui_store_t *store);
void ui_store_reload(ui_store_t *store);
bool ui_store_apply_event(ui_store_t *store, const gw_event_t *event);
bool ui_store_next_group(ui_store_t *store);
bool ui_store_prev_group(ui_store_t *store);
bool ui_store_next_item(ui_store_t *store);
bool ui_store_prev_item(ui_store_t *store);
const ui_group_vm_t *ui_store_active_group(const ui_store_t *store);
const ui_group_item_vm_t *ui_store_active_item(const ui_store_t *store);
