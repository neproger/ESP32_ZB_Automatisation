#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/*
 * MODEL-блоб (статическая часть canonical-модели: devices/entities/capabilities/
 * automations/groups/location/settings) хранится файлом в разделе flash и отдаётся по
 * GET /model.bin. Пересобирается в фоне при изменениях MODEL-типов; новые подключения
 * забирают актуальный файл, а живым клиентам изменения приходят как WS-дельты.
 */

void web_model_file_init(void);

/* Пометить файл «грязным» (вызывается при изменении MODEL-типа). Пересборка — в фоне. */
void web_model_file_mark_dirty(void);

/* Отдать активный слот по GET /model.bin (chunked). */
esp_err_t web_model_file_serve(httpd_req_t *req);
