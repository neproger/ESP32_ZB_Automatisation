#include "gw_core/runtime_sync.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"

#include "gw_core/device_registry.h"
#include "gw_core/device_storage.h"
#include "gw_core/event_bus.h"
#include "gw_core/sensor_store.h"
#include "gw_core/state_store.h"
#include "gw_core/zb_model.h"

static const char *TAG = "gw_runtime_sync";
static bool s_inited;
static bool s_snapshot_active;
static gw_device_uid_t s_snapshot_stale[GW_DEVICE_MAX_DEVICES];
static size_t s_snapshot_stale_count;

static bool snapshot_uid_equals(const gw_device_uid_t *a, const gw_device_uid_t *b)
{
    if (!a || !b) {
        return false;
    }
    return strncmp(a->uid, b->uid, sizeof(a->uid)) == 0;
}

static void snapshot_stale_remove_uid(const gw_device_uid_t *uid)
{
    if (!uid || uid->uid[0] == '\0') {
        return;
    }
    for (size_t i = 0; i < s_snapshot_stale_count; i++) {
        if (snapshot_uid_equals(&s_snapshot_stale[i], uid)) {
            for (size_t j = i + 1; j < s_snapshot_stale_count; j++) {
                s_snapshot_stale[j - 1] = s_snapshot_stale[j];
            }
            s_snapshot_stale_count--;
            return;
        }
    }
}

static bool is_event_type(const char *type, const char *lhs, const char *rhs)
{
    if (!type) {
        return false;
    }
    return (strcmp(type, lhs) == 0) || (strcmp(type, rhs) == 0);
}

static bool resolve_uid(const gw_event_t *e, gw_device_uid_t *out_uid)
{
    if (!e || !out_uid) {
        return false;
    }
    memset(out_uid, 0, sizeof(*out_uid));

    if (e->device_uid[0] != '\0') {
        strlcpy(out_uid->uid, e->device_uid, sizeof(out_uid->uid));
        return true;
    }
    if (e->short_addr != 0 && gw_zb_model_find_uid_by_short(e->short_addr, out_uid)) {
        return true;
    }
    return false;
}


static bool value_as_bool(const gw_event_t *e, bool *out)
{
    if (!e || !out) {
        return false;
    }
    switch ((gw_event_value_type_t)e->payload_value_type) {
        case GW_EVENT_VALUE_BOOL:
            *out = (e->payload_value_bool != 0);
            return true;
        case GW_EVENT_VALUE_I64:
            *out = (e->payload_value_i64 != 0);
            return true;
        case GW_EVENT_VALUE_F64:
            *out = (e->payload_value_f64 != 0.0);
            return true;
        default:
            return false;
    }
}

static bool value_as_i64(const gw_event_t *e, int64_t *out)
{
    if (!e || !out) {
        return false;
    }
    switch ((gw_event_value_type_t)e->payload_value_type) {
        case GW_EVENT_VALUE_I64:
            *out = e->payload_value_i64;
            return true;
        case GW_EVENT_VALUE_BOOL:
            *out = e->payload_value_bool ? 1 : 0;
            return true;
        case GW_EVENT_VALUE_F64:
            *out = (int64_t)e->payload_value_f64;
            return true;
        default:
            return false;
    }
}

static void process_attr_report(const gw_device_uid_t *uid, const gw_event_t *e)
{
    if (!uid || uid->uid[0] == '\0' || !e) {
        return;
    }
    if (!(e->payload_flags & GW_EVENT_PAYLOAD_HAS_CLUSTER) || !(e->payload_flags & GW_EVENT_PAYLOAD_HAS_ATTR)) {
        return;
    }

    const uint16_t cluster = e->payload_cluster;
    const uint16_t attr = e->payload_attr;

    // Temperature (0x0402/0x0000), hundredths of Celsius.
    if (cluster == 0x0402 && attr == 0x0000) {
        gw_sensor_value_t v = {0};
        v.uid = *uid;
        v.short_addr = e->short_addr;
        v.endpoint = e->payload_endpoint;
        v.cluster_id = cluster;
        v.attr_id = attr;
        v.ts_ms = e->ts_ms;
        float celsius = 0.0f;

        if ((gw_event_value_type_t)e->payload_value_type == GW_EVENT_VALUE_F64) {
            celsius = (float)e->payload_value_f64;
            v.value_type = GW_SENSOR_VALUE_I32;
            v.value_i32 = (int32_t)(celsius * 100.0f);
            (void)gw_sensor_store_upsert(&v);
            (void)gw_state_store_set_f32(uid, "temperature_c", celsius, e->ts_ms);
        } else {
            int64_t raw = 0;
            if (value_as_i64(e, &raw)) {
                v.value_type = GW_SENSOR_VALUE_I32;
                v.value_i32 = (int32_t)raw;
                (void)gw_sensor_store_upsert(&v);
                (void)gw_state_store_set_f32(uid, "temperature_c", ((float)v.value_i32) / 100.0f, e->ts_ms);
            }
        }
        return;
    }

    // Humidity (0x0405/0x0000), hundredths of percent.
    if (cluster == 0x0405 && attr == 0x0000) {
        gw_sensor_value_t v = {0};
        v.uid = *uid;
        v.short_addr = e->short_addr;
        v.endpoint = e->payload_endpoint;
        v.cluster_id = cluster;
        v.attr_id = attr;
        v.ts_ms = e->ts_ms;
        float humidity = 0.0f;

        if ((gw_event_value_type_t)e->payload_value_type == GW_EVENT_VALUE_F64) {
            humidity = (float)e->payload_value_f64;
            v.value_type = GW_SENSOR_VALUE_U32;
            v.value_u32 = (uint32_t)(humidity * 100.0f);
            (void)gw_sensor_store_upsert(&v);
            (void)gw_state_store_set_f32(uid, "humidity_pct", humidity, e->ts_ms);
        } else {
            int64_t raw = 0;
            if (value_as_i64(e, &raw) && raw >= 0) {
                v.value_type = GW_SENSOR_VALUE_U32;
                v.value_u32 = (uint32_t)raw;
                (void)gw_sensor_store_upsert(&v);
                (void)gw_state_store_set_f32(uid, "humidity_pct", ((float)v.value_u32) / 100.0f, e->ts_ms);
            }
        }
        return;
    }

    // Battery percentage (0x0001/0x0021).
    if (cluster == 0x0001 && attr == 0x0021) {
        int64_t pct = 0;
        if (value_as_i64(e, &pct) && pct >= 0) {
            gw_sensor_value_t v = {0};
            v.uid = *uid;
            v.short_addr = e->short_addr;
            v.endpoint = e->payload_endpoint;
            v.cluster_id = cluster;
            v.attr_id = attr;
            v.value_type = GW_SENSOR_VALUE_U32;
            v.value_u32 = (uint32_t)pct;
            v.ts_ms = e->ts_ms;
            (void)gw_sensor_store_upsert(&v);
            (void)gw_state_store_set_u32(uid, "battery_pct", (uint32_t)pct, e->ts_ms);
        }
        return;
    }

    // Battery voltage (0x0001/0x0020), normalize to millivolts.
    if (cluster == 0x0001 && attr == 0x0020) {
        int64_t raw = 0;
        if (value_as_i64(e, &raw) && raw >= 0) {
            uint32_t mv = (uint32_t)raw;
            gw_sensor_value_t v = {0};
            v.uid = *uid;
            v.short_addr = e->short_addr;
            v.endpoint = e->payload_endpoint;
            v.cluster_id = cluster;
            v.attr_id = attr;
            v.value_type = GW_SENSOR_VALUE_U32;
            v.value_u32 = mv;
            v.ts_ms = e->ts_ms;
            (void)gw_sensor_store_upsert(&v);
            (void)gw_state_store_set_u32(uid, "battery_mv", mv, e->ts_ms);
        }
        return;
    }

    // On/off (0x0006/0x0000).
    if (cluster == 0x0006 && attr == 0x0000) {
        bool onoff = false;
        if (value_as_bool(e, &onoff)) {
            (void)gw_state_store_set_bool(uid, "onoff", onoff, e->ts_ms);
        }
        return;
    }

    // Current level (0x0008/0x0000).
    if (cluster == 0x0008 && attr == 0x0000) {
        int64_t lvl = 0;
        if (value_as_i64(e, &lvl) && lvl >= 0) {
            gw_sensor_value_t v = {0};
            v.uid = *uid;
            v.short_addr = e->short_addr;
            v.endpoint = e->payload_endpoint;
            v.cluster_id = cluster;
            v.attr_id = attr;
            v.value_type = GW_SENSOR_VALUE_U32;
            v.value_u32 = (uint32_t)lvl;
            v.ts_ms = e->ts_ms;
            (void)gw_sensor_store_upsert(&v);
            (void)gw_state_store_set_u32(uid, "level", (uint32_t)lvl, e->ts_ms);
        }
        return;
    }

    // Color X/Y and color temperature (0x0300).
    if (cluster == 0x0300 && (attr == 0x0003 || attr == 0x0004 || attr == 0x0007)) {
        int64_t raw = 0;
        if (value_as_i64(e, &raw) && raw >= 0) {
            gw_sensor_value_t v = {0};
            v.uid = *uid;
            v.short_addr = e->short_addr;
            v.endpoint = e->payload_endpoint;
            v.cluster_id = cluster;
            v.attr_id = attr;
            v.value_type = GW_SENSOR_VALUE_U32;
            v.value_u32 = (uint32_t)raw;
            v.ts_ms = e->ts_ms;
            (void)gw_sensor_store_upsert(&v);
            if (attr == 0x0003) {
                (void)gw_state_store_set_u32(uid, "color_x", (uint32_t)raw, e->ts_ms);
            } else if (attr == 0x0004) {
                (void)gw_state_store_set_u32(uid, "color_y", (uint32_t)raw, e->ts_ms);
            } else {
                (void)gw_state_store_set_u32(uid, "color_temp_mireds", (uint32_t)raw, e->ts_ms);
            }
        }
        return;
    }

    // Occupancy (0x0406/0x0000).
    if (cluster == 0x0406 && attr == 0x0000) {
        bool occ = false;
        if (value_as_bool(e, &occ)) {
            (void)gw_state_store_set_bool(uid, "occupancy", occ, e->ts_ms);
        }
        return;
    }

    // Illuminance raw (0x0400/0x0000).
    if (cluster == 0x0400 && attr == 0x0000) {
        int64_t raw = 0;
        if (value_as_i64(e, &raw) && raw >= 0) {
            (void)gw_state_store_set_u32(uid, "illuminance_raw", (uint32_t)raw, e->ts_ms);
        }
        return;
    }

    // Pressure raw (0x0403/0x0000).
    if (cluster == 0x0403 && attr == 0x0000) {
        int64_t raw = 0;
        if (value_as_i64(e, &raw)) {
            gw_sensor_value_t v = {0};
            v.uid = *uid;
            v.short_addr = e->short_addr;
            v.endpoint = e->payload_endpoint;
            v.cluster_id = cluster;
            v.attr_id = attr;
            v.value_type = GW_SENSOR_VALUE_I32;
            v.value_i32 = (int32_t)raw;
            v.ts_ms = e->ts_ms;
            (void)gw_sensor_store_upsert(&v);
            (void)gw_state_store_set_f32(uid, "pressure_raw", (float)raw, e->ts_ms);
        }
        return;
    }

    // Generic numeric/bool mirror for unsupported attrs.
    char key[40] = {0};
    (void)snprintf(key, sizeof(key), "cluster_%04x_attr_%04x", (unsigned)cluster, (unsigned)attr);
    switch ((gw_event_value_type_t)e->payload_value_type) {
        case GW_EVENT_VALUE_BOOL:
            (void)gw_state_store_set_bool(uid, key, e->payload_value_bool != 0, e->ts_ms);
            break;
        case GW_EVENT_VALUE_F64:
            (void)gw_state_store_set_f32(uid, key, (float)e->payload_value_f64, e->ts_ms);
            break;
        case GW_EVENT_VALUE_I64:
            if (e->payload_value_i64 >= 0) {
                (void)gw_state_store_set_u64(uid, key, (uint64_t)e->payload_value_i64, e->ts_ms);
            } else {
                (void)gw_state_store_set_f32(uid, key, (float)e->payload_value_i64, e->ts_ms);
            }
            break;
        default:
            break;
    }
}

static void runtime_event_listener(const gw_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event) {
        return;
    }

    gw_device_uid_t uid = {0};
    bool have_uid = resolve_uid(event, &uid);

    if (is_event_type(event->type, "device.join", "zigbee.device_join")) {
        (void)have_uid;
        return;
    }

    if (is_event_type(event->type, "device.leave", "zigbee.device_leave")) {
        (void)have_uid;
        return;
    }

    if (is_event_type(event->type, "zigbee.command", "zigbee.cmd")) {
        (void)have_uid;
        return;
    }

    if (strcmp(event->type, "zigbee.attr_report") == 0) {
        if (!have_uid) {
            return;
        }
        process_attr_report(&uid, event);
    }
}

esp_err_t gw_runtime_sync_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t err = gw_event_bus_add_listener(runtime_event_listener, NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "runtime sync initialized");
    return ESP_OK;
}

esp_err_t gw_runtime_sync_snapshot_begin(uint16_t total_devices)
{
    (void)total_devices;
    gw_device_t devices[GW_DEVICE_MAX_DEVICES] = {0};
    size_t count = gw_device_registry_list(devices, GW_DEVICE_MAX_DEVICES);
    s_snapshot_stale_count = 0;
    for (size_t i = 0; i < count && s_snapshot_stale_count < GW_DEVICE_MAX_DEVICES; i++) {
        s_snapshot_stale[s_snapshot_stale_count++] = devices[i].device_uid;
    }
    s_snapshot_active = true;
    ESP_LOGI(TAG, "snapshot begin (stale candidates=%u)", (unsigned)s_snapshot_stale_count);
    return ESP_OK;
}

esp_err_t gw_runtime_sync_snapshot_upsert_device(const gw_device_t *device)
{
    if (!device || device->device_uid.uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_snapshot_active) {
        // Rebuild endpoint list for seen device from fresh snapshot records.
        (void)gw_zb_model_remove_device(&device->device_uid);
        snapshot_stale_remove_uid(&device->device_uid);
    }
    return gw_device_registry_upsert(device);
}

esp_err_t gw_runtime_sync_snapshot_upsert_endpoint(const gw_zb_endpoint_t *endpoint)
{
    if (!endpoint || endpoint->uid.uid[0] == '\0' || endpoint->endpoint == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return gw_zb_model_upsert_endpoint(endpoint);
}

esp_err_t gw_runtime_sync_snapshot_remove_device(const gw_device_uid_t *uid)
{
    if (!uid || uid->uid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    snapshot_stale_remove_uid(uid);
    (void)gw_zb_model_remove_device(uid);
    return gw_device_registry_remove(uid);
}

esp_err_t gw_runtime_sync_snapshot_end(void)
{
    if (!s_snapshot_active) {
        return ESP_OK;
    }
    for (size_t i = 0; i < s_snapshot_stale_count; i++) {
        (void)gw_zb_model_remove_device(&s_snapshot_stale[i]);
        (void)gw_device_registry_remove(&s_snapshot_stale[i]);
    }
    ESP_LOGI(TAG, "snapshot sweep removed=%u", (unsigned)s_snapshot_stale_count);
    s_snapshot_stale_count = 0;
    s_snapshot_active = false;
    ESP_LOGI(TAG, "snapshot end");
    return ESP_OK;
}


