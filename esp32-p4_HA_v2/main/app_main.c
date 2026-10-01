#include <stddef.h>
#include <stdint.h>

#include "domain/domain.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_model/ha_entities.h"
#include "journal_console.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_radio.h"

/*
 * Bootstrap приложения: единственное место, где собраны реальные таблицы, их ёмкости
 * и политика хранения (docs/RECORD_MODEL.md §1.1). Регистрация типов — не сервисная
 * инициализация: `device` и `state` создаёт Zigbee, а читают их все.
 */

#define APP_ENTITY_TYPES 3
#define APP_JOURNAL_CAPACITY 64
#define APP_PAYLOAD_CAPACITY 8
#define APP_PAYLOAD_MAX_SIZE 64

#define APP_DEVICE_CAPACITY 32
#define APP_STATE_CAPACITY 256
#define APP_ENDPOINT_CAPACITY 128

#define DISPATCHER_TASK_STACK 4096
#define DISPATCHER_TASK_PRIORITY 6

static const char *TAG = "bootstrap";

static domain_t s_domain;

/* Устройство переживает перезагрузку: имя задаёт пользователь, модель — Basic-кластер. */
static const domain_entity_desc_t device_desc = {
    .type = (domain_entity_t)HA_ENTITY_DEVICE,
    .key_size = sizeof(ha_device_uid_t),
    .payload_size = sizeof(ha_device_record_t),
    .capacity = APP_DEVICE_CAPACITY,
    .backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH,
    .persist_key = "device",
};

/*
 * Состояние атрибута — RAM: после перезагрузки его приносят репорты, а персистентность
 * здесь означала бы запись во flash на каждый репорт (docs/RECORD_MODEL.md:123-138).
 */
static const domain_entity_desc_t state_desc = {
    .type = (domain_entity_t)HA_ENTITY_STATE,
    .key_size = sizeof(ha_zb_state_key_t),
    .payload_size = sizeof(ha_zb_state_record_t),
    .capacity = APP_STATE_CAPACITY,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

/*
 * Топология: состав кластеров endpoint'а. RAM — интервью повторяется на каждом старте,
 * а персистентность здесь означала бы запись во flash ради данных, которые и так
 * приходят заново.
 */
static const domain_entity_desc_t endpoint_desc = {
    .type = (domain_entity_t)HA_ENTITY_ENDPOINT,
    .key_size = sizeof(ha_endpoint_key_t),
    .payload_size = sizeof(ha_endpoint_record_t),
    .capacity = APP_ENDPOINT_CAPACITY,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

static bool start_step(sys_error_t err, const char *what)
{
    if (sys_failed(err)) {
        ESP_LOGE(TAG, "%s failed: layer=%u code=%u", what, (unsigned)err.layer, (unsigned)err.code);
        return false;
    }
    return true;
}

/*
 * Задача Dispatcher'а: единственный потребитель Journal. Domain задачу не создаёт —
 * платформа даёт только lock и сигнал, поэтому задача принадлежит приложению
 * (docs/domain/DISPATCHER.md §1).
 */
static void dispatcher_task(void *arg)
{
    domain_t *domain = (domain_t *)arg;

    for (;;) {
        bool signalled = false;
        domain_dispatch_wait(domain, portMAX_DELAY, &signalled);

        size_t delivered = 0;
        domain_dispatch_once(domain, &delivered);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "HA on ESP32-P4: bootstrap");

    if (!start_step(domain_init(&s_domain, APP_ENTITY_TYPES, APP_JOURNAL_CAPACITY,
                                APP_PAYLOAD_CAPACITY, APP_PAYLOAD_MAX_SIZE),
                    "domain init")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &device_desc), "register device")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &state_desc), "register state")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &endpoint_desc), "register endpoint")) {
        return;
    }
    if (!start_step(journal_console_subscribe(&s_domain), "journal console")) {
        return;
    }

    if (xTaskCreate(dispatcher_task, "dispatcher", DISPATCHER_TASK_STACK, &s_domain,
                    DISPATCHER_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "dispatcher task not created");
        return;
    }

    if (!start_step(zigbee_start(&s_domain), "zigbee start")) {
        return;
    }
    if (!start_step(zigbee_radio_start(&s_domain), "zigbee radio")) {
        return;
    }

    ESP_LOGI(TAG, "bootstrap done");
}
