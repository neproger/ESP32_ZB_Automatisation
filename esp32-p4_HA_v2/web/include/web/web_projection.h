#pragma once

#include "domain/domain.h"

/*
 * Canonical Domain → браузер: snapshot и дельты. Типы, отдаваемые наружу, заданы явным
 * списком (SCHEMA) — новая внутренняя таблица Domain не становится публичным API
 * автоматически. Кадры несут canonical key/record verbatim (web_proto).
 */

void web_projection_init(domain_t *domain);

/* Полный snapshot сессии (SYNC_BEGIN … UPSERT × N … SYNC_END). */
void web_projection_snapshot(int fd);

/* Дельта по факту Domain: UPSERT/REMOVE по canonical типу либо EVENT (ha_event_t). */
void web_projection_on_event(const domain_event_t *event);
