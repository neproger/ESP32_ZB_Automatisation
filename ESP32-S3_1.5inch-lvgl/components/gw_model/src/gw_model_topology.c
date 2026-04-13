#include "gw_model/gw_model_topology.h"

#include "gw_model/gw_model_device_meta.h"
#include "gw_store/gw_store_topology.h"

esp_err_t gw_model_init_topology(void)
{
    return gw_store_init_topology();
}

esp_err_t gw_model_deinit_topology(void)
{
    return gw_store_deinit_topology();
}

esp_err_t gw_model_upsert_device(const gw_proto_device_v1_t *record,
                                 bool *out_changed,
                                 bool *out_inserted)
{
    return gw_store_upsert_device(record, out_changed, out_inserted);
}

esp_err_t gw_model_get_device(const gw_device_uid_t *uid,
                              gw_proto_device_v1_t *out_record)
{
    esp_err_t err = gw_store_get_device(uid, out_record);
    if (err == ESP_OK) {
        gw_model_apply_device_meta(out_record);
    }
    return err;
}

esp_err_t gw_model_remove_full_device(const gw_device_uid_t *uid,
                                      bool *out_removed)
{
    esp_err_t err = gw_store_remove_full_device(uid, out_removed);
    if (err == ESP_OK) {
        (void)gw_model_remove_device_meta(uid, NULL);
    }
    return err;
}

esp_err_t gw_model_remove_device(const gw_device_uid_t *uid,
                                 bool *out_removed)
{
    esp_err_t err = gw_store_remove_device(uid, out_removed);
    if (err == ESP_OK) {
        (void)gw_model_remove_device_meta(uid, NULL);
    }
    return err;
}

size_t gw_model_count_devices(void)
{
    return gw_store_count_devices();
}

esp_err_t gw_model_get_device_by_index(size_t index,
                                       gw_proto_device_v1_t *out_record)
{
    esp_err_t err = gw_store_get_device_by_index(index, out_record);
    if (err == ESP_OK) {
        gw_model_apply_device_meta(out_record);
    }
    return err;
}

size_t gw_model_iter_devices(micro_db_iter_cb_t cb, void *user_ctx)
{
    return gw_store_iter_devices(cb, user_ctx);
}

esp_err_t gw_model_upsert_endpoint(const gw_proto_endpoint_v1_t *record,
                                   bool *out_changed,
                                   bool *out_inserted)
{
    return gw_store_upsert_endpoint(record, out_changed, out_inserted);
}

esp_err_t gw_model_get_endpoint(const gw_model_endpoint_key_t *key,
                                gw_proto_endpoint_v1_t *out_record)
{
    return gw_store_get_endpoint((const gw_store_endpoint_key_t *)key, out_record);
}

esp_err_t gw_model_remove_endpoint(const gw_model_endpoint_key_t *key,
                                   bool *out_removed)
{
    return gw_store_remove_endpoint((const gw_store_endpoint_key_t *)key, out_removed);
}

size_t gw_model_count_endpoints(void)
{
    return gw_store_count_endpoints();
}

esp_err_t gw_model_get_endpoint_by_index(size_t index,
                                         gw_proto_endpoint_v1_t *out_record)
{
    return gw_store_get_endpoint_by_index(index, out_record);
}

size_t gw_model_iter_endpoints(micro_db_iter_cb_t cb, void *user_ctx)
{
    return gw_store_iter_endpoints(cb, user_ctx);
}

size_t gw_model_iter_endpoints_for_device(const gw_device_uid_t *uid,
                                          micro_db_iter_cb_t cb,
                                          void *user_ctx)
{
    return gw_store_iter_endpoints_for_device(uid, cb, user_ctx);
}

bool gw_model_find_device_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid)
{
    return gw_store_find_device_uid_by_short(short_addr, out_uid);
}
