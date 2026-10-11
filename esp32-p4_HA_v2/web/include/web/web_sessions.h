#pragma once

#include <stdint.h>

#include "esp_http_server.h"

/*
 * Надёжная доставка кадров каждому WS-клиенту: per-client очередь + stop-and-wait
 * (транспортом присвоенный seq, ACK, retry, bounded). Транспорт тупой — знает только
 * байты и seq, тип кадра не интерпретирует.
 */

void web_sessions_init(httpd_handle_t server);

/* Зарегистрировать/сбросить сессию по fd (вызывается на WS handshake). */
void web_sessions_open(int fd);

/* Клиент подтвердил кадр с данным header.seq. */
void web_sessions_ack(int fd, uint16_t seq);

/* Поставить кадр (type + payload) в очередь клиента; не шлёт. */
void web_sessions_send(int fd, uint8_t type, const void *payload, uint16_t len);

/* То же — всем активным сессиям. */
void web_sessions_broadcast(uint8_t type, const void *payload, uint16_t len);

/* Продвинуть отправку: шлёт head ожидающим, ретраит по таймауту, закрывает мёртвых. */
void web_sessions_step(void);
