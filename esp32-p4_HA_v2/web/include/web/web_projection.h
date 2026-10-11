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

/* STATE-only snapshot (без MODEL): SYNC_BEGIN … STATE × N … SYNC_END. */
void web_projection_send_state(int fd);

/* true — тип относится к MODEL (статическая часть: файл/GET). */
bool web_projection_is_model(domain_entity_t type);

/*
 * Собирает MODEL-блоб (records only, [type][key][record] × N) в buf. Возвращает размер
 * или 0 при переполнении cap; *out_count — число записей.
 */
size_t web_projection_build_model(uint8_t *buf, size_t cap, uint32_t *out_count);

/* Дельта по факту Domain: UPSERT/REMOVE по canonical типу либо EVENT (ha_event_t). */
void web_projection_on_event(const domain_event_t *event);
