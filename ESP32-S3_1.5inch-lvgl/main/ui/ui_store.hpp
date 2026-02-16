#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gw_core/device_registry.h"
#include "gw_core/event_bus.h"
#include "gw_core/state_store.h"
#include "gw_core/zb_model.h"
#include "ui_mapper.hpp"

static constexpr size_t UI_STORE_DEVICE_CAP = 64;
static constexpr size_t UI_STORE_ENDPOINT_CAP = 8;

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
    bool has_color_temp_mireds;
    uint16_t color_temp_mireds;
} ui_endpoint_vm_t;

typedef struct
{
    gw_device_uid_t uid;
    uint16_t short_addr;
    char name[32];
    size_t active_endpoint_idx;
    size_t endpoint_count;
    ui_endpoint_vm_t endpoints[UI_STORE_ENDPOINT_CAP];
} ui_device_vm_t;

typedef struct
{
    size_t device_count;
    size_t active_device_idx;
    ui_device_vm_t devices[UI_STORE_DEVICE_CAP];
} ui_store_t;

void ui_store_init(ui_store_t *store);
void ui_store_reload(ui_store_t *store);
bool ui_store_apply_event(ui_store_t *store, const gw_event_t *event);
bool ui_store_next_device(ui_store_t *store);
bool ui_store_prev_device(ui_store_t *store);
bool ui_store_next_endpoint(ui_store_t *store);
bool ui_store_prev_endpoint(ui_store_t *store);
const ui_device_vm_t *ui_store_active_device(const ui_store_t *store);
