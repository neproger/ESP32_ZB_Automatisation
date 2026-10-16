#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/*
 * MODEL-блоб (статическая часть canonical-модели: devices/entities/capabilities/
 * automations/groups/location/settings) собирается в PSRAM целиком и отдаётся по
 * GET /model.bin. Пересобирается лениво — при первом запросе после изменения MODEL-типа;
 * живым клиентам изменения приходят как WS-дельты.
 */

void web_model_file_init(void);

/* Пометить блоб «грязным» (вызывается при изменении MODEL-типа); пересборка — при запросе. */
void web_model_file_mark_dirty(void);

/* Собрать (если нужно) и отдать блоб по GET /model.bin. */
esp_err_t web_model_file_serve(httpd_req_t *req);
