#include "web/web_sessions.h"

#include <string.h>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web/web_proto.h"

#define WEB_MAX_CLIENTS 8
#define WEB_TX_MAX_FRAMES 64
#define WEB_TX_MAX_BYTES (16u * 1024u)
#define WEB_TX_ACK_TIMEOUT_MS 1500u
#define WEB_TX_MAX_RETRY 3u

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
} web_client_t;

static httpd_handle_t s_server;
static web_client_t s_clients[WEB_MAX_CLIENTS];

static web_client_t *client_find(int fd)
{
    for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
        if (s_clients[i].used && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static void client_clear(web_client_t *c)
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

/* Слишком медленный/мёртвый клиент: закрыть сессию, очередь отбросить. */
static void client_kill(web_client_t *c)
{
    const int fd = c->fd;
    client_clear(c);
    c->used = false;
    (void)httpd_sess_trigger_close(s_server, fd);
}

static web_client_t *client_open(int fd)
{
    web_client_t *c = client_find(fd);
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
    client_clear(c);
    c->used = true;
    c->fd = fd;
    return c;
}

static void web_tx_ack(int fd, uint16_t seq)
{
    web_client_t *c = client_find(fd);
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

static void web_tx_enqueue(int fd, const uint8_t *frame, uint16_t len)
{
    if (len < WEB_PROTO_HDR_SIZE) {
        return;
    }
    web_client_t *c = client_find(fd);
    if (c == NULL) {
        c = client_open(fd);
        if (c == NULL) {
            return;
        }
    }
    if (c->frames >= WEB_TX_MAX_FRAMES || c->bytes + len > WEB_TX_MAX_BYTES) {
        client_kill(c); /* клиент не успевает: сессию закрываем, reconnect → snapshot */
        return;
    }
    web_tx_node_t *n = (web_tx_node_t *)pvPortMalloc(sizeof(web_tx_node_t));
    if (n == NULL) {
        client_kill(c);
        return;
    }
    n->next = NULL;
    n->len = len;
    n->seq = c->next_seq++;
    memcpy(n->data, frame, len);
    n->data[4] = (uint8_t)(n->seq & 0xffu); /* header.seq = транспортом присвоенный */
    n->data[5] = (uint8_t)(n->seq >> 8);
    if (c->tail != NULL) {
        c->tail->next = n;
    } else {
        c->head = n;
    }
    c->tail = n;
    c->frames++;
    c->bytes += len;
}

static bool client_alive(const web_client_t *c)
{
    return httpd_ws_get_fd_info(s_server, c->fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

static void web_tx_send_head(web_client_t *c)
{
    httpd_ws_frame_t ws = {
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = c->head->data,
        .len = c->head->len,
    };
    if (httpd_ws_send_frame_async(s_server, c->fd, &ws) != ESP_OK) {
        client_kill(c);
        return;
    }
    c->waiting = true;
    c->waiting_seq = c->head->seq;
    c->sent_at = xTaskGetTickCount();
}

static void web_tx_step(void)
{
    for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
        web_client_t *c = &s_clients[i];
        if (!c->used) {
            continue;
        }
        if (!client_alive(c)) {
            client_clear(c);
            c->used = false;
            continue;
        }
        if (c->head == NULL) {
            c->waiting = false;
            continue;
        }
        if (!c->waiting) {
            c->retry = 0;
            web_tx_send_head(c);
            continue;
        }
        const uint32_t elapsed_ms =
            (uint32_t)(xTaskGetTickCount() - c->sent_at) * portTICK_PERIOD_MS;
        if (elapsed_ms < WEB_TX_ACK_TIMEOUT_MS) {
            continue;
        }
        if (c->retry >= WEB_TX_MAX_RETRY) {
            client_kill(c); /* ACK не пришёл после ретраев: клиент мёртв */
            continue;
        }
        c->retry++;
        web_tx_send_head(c);
    }
}

void web_sessions_init(httpd_handle_t server)
{
    s_server = server;
}

void web_sessions_open(int fd)
{
    (void)client_open(fd);
}

void web_sessions_ack(int fd, uint16_t seq)
{
    web_tx_ack(fd, seq);
}

void web_sessions_send(int fd, uint8_t type, const void *payload, uint16_t len)
{
    uint8_t frame[WEB_PROTO_MAX_FRAME];
    const size_t n = web_frame_encode(frame, sizeof(frame), type, 0, payload, len);
    if (n == 0) {
        return;
    }
    web_tx_enqueue(fd, frame, (uint16_t)n);
}

void web_sessions_broadcast(uint8_t type, const void *payload, uint16_t len)
{
    for (size_t i = 0; i < WEB_MAX_CLIENTS; i++) {
        if (s_clients[i].used) {
            web_sessions_send(s_clients[i].fd, type, payload, len);
        }
    }
}

void web_sessions_step(void)
{
    web_tx_step();
}
