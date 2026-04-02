#include "gw_model/gw_model.h"

#include "gw_model/gw_model_groups.h"
#include "gw_model/gw_model_automation.h"
#include "gw_model/gw_model_settings.h"
#include "gw_model/gw_model_state.h"
#include "gw_model/gw_model_sync.h"
#include "gw_model/gw_model_topology.h"

esp_err_t gw_model_init(void)
{
    esp_err_t err = gw_model_init_topology();
    if (err != ESP_OK) {
        return err;
    }

    err = gw_model_init_state();
    if (err != ESP_OK) {
        (void)gw_model_deinit_topology();
        return err;
    }

    err = gw_model_init_groups();
    if (err != ESP_OK) {
        (void)gw_model_deinit_state();
        (void)gw_model_deinit_topology();
        return err;
    }

    err = gw_model_init_settings();
    if (err != ESP_OK) {
        (void)gw_model_deinit_groups();
        (void)gw_model_deinit_state();
        (void)gw_model_deinit_topology();
        return err;
    }

    err = gw_model_init_automation();
    if (err != ESP_OK) {
        (void)gw_model_deinit_settings();
        (void)gw_model_deinit_groups();
        (void)gw_model_deinit_state();
        (void)gw_model_deinit_topology();
        return err;
    }

    err = gw_model_sync_init();
    if (err != ESP_OK) {
        (void)gw_model_deinit_automation();
        (void)gw_model_deinit_settings();
        (void)gw_model_deinit_groups();
        (void)gw_model_deinit_state();
        (void)gw_model_deinit_topology();
        return err;
    }

    return ESP_OK;
}

esp_err_t gw_model_deinit(void)
{
    (void)gw_model_sync_deinit();
    (void)gw_model_deinit_automation();
    (void)gw_model_deinit_settings();
    (void)gw_model_deinit_groups();
    (void)gw_model_deinit_state();
    (void)gw_model_deinit_topology();
    return ESP_OK;
}
