#include "ui_events_bridge.hpp"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace
{
static QueueHandle_t s_q = nullptr;
static bool s_listener_registered = false;

bool ui_event_is_relevant(const gw_event_t *event)
{
    if (!event || !event->type[0])
    {
        return false;
    }
    if (strcmp(event->type, "zigbee.attr_report") == 0 || strcmp(event->type, "zigbee.attr_read") == 0)
    {
        return true;
    }
    if (strcmp(event->type, "device.join") == 0 || strcmp(event->type, "device.leave") == 0 || strcmp(event->type, "device.changed") == 0)
    {
        return true;
    }
    return false;
}

void event_listener(const gw_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event || !s_q)
    {
        return;
    }
    if (!ui_event_is_relevant(event))
    {
        return;
    }
    gw_event_t copy = *event;
    (void)xQueueSend(s_q, &copy, 0);
}

void ensure_listener_registered()
{
    if (s_listener_registered)
    {
        return;
    }
    const esp_err_t err = gw_event_bus_add_listener(event_listener, nullptr);
    if (err == ESP_OK)
    {
        s_listener_registered = true;
    }
}
} // namespace

esp_err_t ui_events_bridge_init(void)
{
    if (s_q)
    {
        return ESP_OK;
    }
    s_q = xQueueCreate(8, sizeof(gw_event_t));
    if (!s_q)
    {
        return ESP_ERR_NO_MEM;
    }
    // Event bus may be initialized later than UI init. Register lazily.
    ensure_listener_registered();
    return ESP_OK;
}

size_t ui_events_bridge_drain(gw_event_t *out, size_t max_out)
{
    ensure_listener_registered();

    if (!s_q || !out || max_out == 0)
    {
        return 0;
    }
    size_t n = 0;
    while (n < max_out && xQueueReceive(s_q, &out[n], 0) == pdTRUE)
    {
        ++n;
    }
    return n;
}
