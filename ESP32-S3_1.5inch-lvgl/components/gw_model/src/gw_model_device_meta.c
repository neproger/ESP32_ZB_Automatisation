#include "gw_model/gw_model_device_meta.h"

#include <string.h>

#include "micro_db/micro_db_core.h"
#include "gw_model/gw_model_topology.h"
#include "gw_model_notify.h"

static micro_db_table_t s_meta_table;

esp_err_t gw_model_init_device_meta(void)
{
    return micro_db_table_init(&s_meta_table, &GW_MODEL_SCHEMA_DEVICE_META);
}

esp_err_t gw_model_deinit_device_meta(void)
{
    return micro_db_table_deinit(&s_meta_table);
}

esp_err_t gw_model_get_device_meta(const gw_device_uid_t *uid,
                                   gw_model_device_meta_t *out_meta)
{
    if (!uid || !uid->uid[0] || !out_meta) {
        return ESP_ERR_INVALID_ARG;
    }
    return micro_db_table_get(&s_meta_table, uid, out_meta);
}

esp_err_t gw_model_remove_device_meta(const gw_device_uid_t *uid,
                                      bool *out_removed)
{
    if (!uid || !uid->uid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return micro_db_table_remove(&s_meta_table, uid, out_removed);
}

void gw_model_apply_device_meta(gw_proto_device_v1_t *device)
{
    if (!device || !device->device_uid.uid[0]) {
        return;
    }

    gw_model_device_meta_t meta = {0};
    if (gw_model_get_device_meta(&device->device_uid, &meta) == ESP_OK) {
        strlcpy(device->name, meta.name, sizeof(device->name));
        return;
    }

    device->name[0] = '\0';
}

esp_err_t gw_model_set_device_name(const gw_device_uid_t *uid,
                                   const char *name,
                                   bool *out_changed)
{
    if (!uid || !uid->uid[0] || !name) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_model_device_meta_t meta = {
        .uid = *uid,
    };
    strlcpy(meta.name, name, sizeof(meta.name));

    bool changed = false;
    esp_err_t err = micro_db_table_upsert(&s_meta_table, &meta, out_changed ? out_changed : &changed, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const bool did_change = out_changed ? *out_changed : changed;
    if (did_change) {
        gw_proto_device_v1_t device = {0};
        if (gw_model_get_device(uid, &device) == ESP_OK) {
            gw_model_apply_device_meta(&device);
            (void)gw_model_notify_device_upsert(&device);
        }
    }

    return ESP_OK;
}
