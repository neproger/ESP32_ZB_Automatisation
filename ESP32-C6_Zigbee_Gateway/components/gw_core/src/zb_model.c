#include "gw_core/zb_model.h"

#include "gw_core/c6_store.h"

esp_err_t gw_zb_model_init(void)
{
    return gw_c6_store_init();
}

esp_err_t gw_zb_model_upsert_endpoint(const gw_zb_endpoint_t *ep)
{
    return gw_c6_store_endpoint_upsert(ep);
}

esp_err_t gw_zb_model_remove_device(const gw_device_uid_t *uid)
{
    return gw_c6_store_device_remove(uid);
}

size_t gw_zb_model_list_endpoints(const gw_device_uid_t *uid, gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return gw_c6_store_endpoint_list(uid, out_eps, max_eps);
}

size_t gw_zb_model_list_all_endpoints(gw_zb_endpoint_t *out_eps, size_t max_eps)
{
    return gw_c6_store_endpoint_list_all(out_eps, max_eps);
}

bool gw_zb_model_find_uid_by_short(uint16_t short_addr, gw_device_uid_t *out_uid)
{
    return gw_c6_store_find_uid_by_short(short_addr, out_uid);
}
