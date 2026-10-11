#include "web/web_sessions.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "web/web_proto.h"

/*
 * Per-client доставка: очередь + stop-and-wait. Таблицу клиентов и очереди трогают и
 * httpd-задача (open/ack), и задача web (send/step) — доступ под мьютексом.
 * ВАЖНО: httpd-вызовы (send_frame_async / sess_trigger_close / ws_get_fd_info)
 * выполняются ВНЕ мьютекса — иначе возможен дедлок с httpd-lock.
 */

#define WEB_MAX_CLIENTS 8
#define WEB_TX_MAX_FRAMES 64
#define WEB_TX_MAX_BYTES (16u * 1024u)
#define WEB_TX_ACK_TIMEOUT_MS 1500u
#define WEB_TX_MAX_RETRY 3u
#define WEB_OPEN_GRACE_MS 1000u

static const char *TAG = "web_sess";

typedef struct web_tx_node {
    struct web_tx_node *next;
    uint16_t len;
    uint16_t seq;
    uint8_t data[WEB_PROTO_MAX_FRAME];
} web_tx_node_t;

typedef struct {
    bool used;
    int fd;
    web_tx_node_t *head;
    web_tx_node_t *tail;
    size_t frames;
    size_t bytes;
    uint16_t next_seq;
    bool waiting;
    uint16_t waiting_seq;
    uint8_t retry;
    TickType_t sent_at;
    TickType_t opened_at;
} web_client_t;

static httpd_handle_t s_server;
static web_client_t s_clients[WEB_MAX_CLIENTS];
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static web_client_t *client_find_locked(int fd)
{
    for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
        if (s_clients[i].used && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static void client_clear_locked(web_client_t *c)
{
    web_tx_node_t *n = c->head;
    while (n != NULL) {
        web_tx_node_t *next = n->next;
        vPortFree(n);
        n = next;
    }
    const int fd = c->fd;
    memset(c, 0, sizeof(*c));
    c->fd = fd;
}

/* Помечает клиента свободным; возвращает fd для закрытия (вне лока) или -1. */
static int client_drop_locked(web_client_t *c)
{
    if (!c->used) {
        return -1;
    }
    const int fd = c->fd;
    client_clear_locked(c);
    c->used = false;
    return fd;
}

static web_client_t *client_open_locked(int fd)
{
    web_client_t *c = client_find_locked(fd);
    if (c == NULL) {
        for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
            if (!s_clients[i].used) {
                c = &s_clients[i];
                break;
            }
        }
    }
    if (c == NULL) {
        return NULL;
    }
    client_clear_locked(c);
    c->used = true;
    c->fd = fd;
    c->opened_at = xTaskGetTickCount();
    return c;
}

static void web_tx_ack_locked(int fd, uint16_t seq)
{
    web_client_t *c = client_find_locked(fd);
    if (c == NULL || !c->waiting || c->waiting_seq != seq) {
        return;
    }
    web_tx_node_t *n = c->head;
    if (n != NULL) {
        c->head = n->next;
        if (c->head == NULL) {
            c->tail = NULL;
        }
        c->frames--;
        c->bytes -= n->len;
        vPortFree(n);
    }
    c->waiting = false;
    c->retry = 0;
}

/* Возвращает fd для закрытия (вне лока) при переполнении/OOM, иначе -1. */
static int web_tx_enqueue_locked(int fd, const uint8_t *frame, uint16_t len)
{
    if (len < WEB_PROTO_HDR_SIZE) {
        return -1;
    }
    web_client_t *c = client_find_locked(fd);
    if (c == NULL) {
        c = client_open_locked(fd);
        if (c == NULL) {
            return -1;
        }
    }
    if (c->frames >= WEB_TX_MAX_FRAMES || c->bytes + len > WEB_TX_MAX_BYTES) {
        return client_drop_locked(c);
    }
    web_tx_node_t *n = (web_tx_node_t *)pvPortMalloc(sizeof(web_tx_node_t));
    if (n == NULL) {
        return client_drop_locked(c);
    }
    n->next = NULL;
    n->len = len;
    n->seq = c->next_seq++;
    memcpy(n->data, frame, len);
    n->data[4] = (uint8_t)(n->seq & 0xffu);
    n->data[5] = (uint8_t)(n->seq >> 8);
    if (c->tail != NULL) {
        c->tail->next = n;
    } else {
        c->head = n;
    }
    c->tail = n;
    c->frames++;
    c->bytes += len;
    return -1;
}

void web_sessions_init(httpd_handle_t server)
{
    s_server = server;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

void web_sessions_open(int fd)
{
    if (s_lock == NULL) {
        return;
    }
    lock();
    (void)client_open_locked(fd);
    unlock();
}

void web_sessions_ack(int fd, uint16_t seq)
{
    if (s_lock == NULL) {
        return;
    }
    lock();
    web_tx_ack_locked(fd, seq);
    unlock();
}

void web_sessions_send(int fd, uint8_t type, const void *payload, uint16_t len)
{
    if (s_lock == NULL) {
        return;
    }
    uint8_t frame[WEB_PROTO_MAX_FRAME];
    const size_t n = web_frame_encode(frame, sizeof(frame), type, 0, payload, len);
    if (n == 0) {
        return;
    }
    lock();
    const int close_fd = web_tx_enqueue_locked(fd, frame, (uint16_t)n);
    unlock();
    if (close_fd >= 0) {
        (void)httpd_sess_trigger_close(s_server, close_fd);
    }
}

void web_sessions_broadcast(uint8_t type, const void *payload, uint16_t len)
{
    if (s_lock == NULL) {
        return;
    }
    uint8_t frame[WEB_PROTO_MAX_FRAME];
    const size_t n = web_frame_encode(frame, sizeof(frame), type, 0, payload, len);
    if (n == 0) {
        return;
    }
    int close_fds[WEB_MAX_CLIENTS];
    size_t close_count = 0;

    lock();
    for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
        if (!s_clients[i].used) {
            continue;
        }
        const int cf = web_tx_enqueue_locked(s_clients[i].fd, frame, (uint16_t)n);
        if (cf >= 0) {
            close_fds[close_count++] = cf;
        }
    }
    unlock();

    for (size_t i = 0; i < close_count; i++) {
        (void)httpd_sess_trigger_close(s_server, close_fds[i]);
    }
}

void web_sessions_step(void)
{
    if (s_lock == NULL) {
        return;
    }

    for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
        web_client_t *c = &s_clients[i];

        lock();
        if (!c->used) {
            unlock();
            continue;
        }
        const int fd = c->fd;
        const bool head_present = (c->head != NULL);
        const bool waiting = c->waiting;
        const uint8_t retry = c->retry;
        const TickType_t sent_at = c->sent_at;
        const TickType_t opened_at = c->opened_at;
        unlock();

        const bool past_grace =
            (xTaskGetTickCount() - opened_at) * portTICK_PERIOD_MS >= WEB_OPEN_GRACE_MS;
        const bool alive = (httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET);
        if (!alive && !head_present && !past_grace) {
            continue; /* handshake ещё завершается */
        }
        if (!alive) {
            lock();
            const int cf = client_drop_locked(c);
            unlock();
            if (cf >= 0) {
                (void)httpd_sess_trigger_close(s_server, cf);
            }
            continue;
        }

        if (!head_present) {
            lock();
            if (c->used && c->fd == fd) {
                c->waiting = false;
            }
            unlock();
            continue;
        }

        bool need_send = false;
        if (!waiting) {
            need_send = true;
        } else {
            const uint32_t elapsed_ms =
                (uint32_t)(xTaskGetTickCount() - sent_at) * portTICK_PERIOD_MS;
            if (elapsed_ms >= WEB_TX_ACK_TIMEOUT_MS) {
                if (retry >= WEB_TX_MAX_RETRY) {
                    lock();
                    const int cf = client_drop_locked(c);
                    unlock();
                    if (cf >= 0) {
                        (void)httpd_sess_trigger_close(s_server, cf);
                    }
                    continue;
                }
                need_send = true;
            }
        }

        if (need_send) {
            uint8_t local[WEB_PROTO_MAX_FRAME];
            uint16_t send_len = 0;

            lock();
            if (!c->used || c->fd != fd || c->head == NULL) {
                unlock();
                continue;
            }
            if (waiting) {
                c->retry++;
            }
            send_len = c->head->len;
            memcpy(local, c->head->data, send_len);
            c->waiting = true;
            c->waiting_seq = c->head->seq;
            c->sent_at = xTaskGetTickCount();
            unlock();

            httpd_ws_frame_t ws = {
                .type = HTTPD_WS_TYPE_BINARY,
                .payload = local,
                .len = send_len,
            };
            if (httpd_ws_send_frame_async(s_server, fd, &ws) != ESP_OK) {
                lock();
                const int cf = (c->used && c->fd == fd) ? client_drop_locked(c) : -1;
                unlock();
                if (cf >= 0) {
                    (void)httpd_sess_trigger_close(s_server, cf);
                }
            }
        }
    }
}
