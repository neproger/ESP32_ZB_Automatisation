#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "automation/automation.h"
#include "display_p4/display_p4.h"
#include "domain/domain.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_model/ha_capabilities.h"
#include "ha_model/ha_commands.h"
#include "ha_model/ha_entities.h"
#include "ha_model/ha_entity.h"
#include "ha_model/ha_groups.h"
#include "ha_model/ha_rule.h"
#include "ha_model/ha_settings.h"
#include "ha_model/ha_state.h"
#include "ha_model/ha_weather.h"
#include "ha_model/ha_wifi.h"
#include "journal_console.h"
#include "system/system.h"
#include "web/web.h"
#include "wifi/wifi.h"
#include "zigbee/zigbee.h"
#include "zigbee/zigbee_radio.h"

/*
 * Bootstrap приложения: единственное место, где собраны реальные таблицы, их ёмкости
 * и политика хранения (docs/RECORD_MODEL.md §1.1). Регистрация типов — не сервисная
 * инициализация: `device` и `state` создаёт Zigbee, а читают их все.
 */

#define APP_ENTITY_TYPES 16
#define APP_JOURNAL_CAPACITY 64
#define APP_PAYLOAD_CAPACITY 8
#define APP_PAYLOAD_MAX_SIZE 64

#define APP_DEVICE_CAPACITY 32
#define APP_STATE_CAPACITY 256
#define APP_PROPERTY_STATE_CAPACITY 256
#define APP_CAPABILITIES_CAPACITY 32
#define APP_ENDPOINT_CAPACITY 128
#define APP_AUTOMATION_CAPACITY 32
#define APP_DEVICE_REMOVE_CAPACITY 32
#define APP_LOCATION_CAPACITY 4
#define APP_GROUP_CAPACITY 16
#define APP_GROUP_ITEM_CAPACITY 128
#define APP_WEATHER_CAPACITY 1
#define APP_WIFI_SCAN_CAPACITY 1
#define APP_WIFI_KNOWN_CAPACITY 8
#define APP_WIFI_STATUS_CAPACITY 1
#define APP_SETTINGS_CAPACITY 1
#define APP_ENTITY_CAPACITY 32

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
    .type = (domain_entity_t)HA_ENTITY_ZB_DIAG_STATE,
    .key_size = sizeof(ha_zb_state_key_t),
    .payload_size = sizeof(ha_zb_state_record_t),
    .capacity = APP_STATE_CAPACITY,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

/*
 * Топология: состав кластеров endpoint'а. FLASH: у устройства endpoint'ы не меняются,
 * поэтому интервью сохраняется один раз и переживает перезагрузку (интервью обновляет
 * запись при повторном join). Состояние атрибутов при этом остаётся RAM.
 */
static const domain_entity_desc_t endpoint_desc = {
    .type = (domain_entity_t)HA_ENTITY_ENDPOINT,
    .key_size = sizeof(ha_endpoint_key_t),
    .payload_size = sizeof(ha_endpoint_record_t),
    .capacity = APP_ENDPOINT_CAPACITY,
    .backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH,
    .persist_key = "endpoint",
};

/*
 * Список на удаление: устройство нельзя убрать из сети напрямую, поэтому помечаем его
 * и шлём leave, когда оно появится. FLASH — пометка должна пережить перезагрузку.
 */
static const domain_entity_desc_t device_remove_desc = {
    .type = (domain_entity_t)HA_ENTITY_DEVICE_REMOVE,
    .key_size = sizeof(ha_device_uid_t),
    .payload_size = sizeof(ha_device_remove_record_t),
    .capacity = APP_DEVICE_REMOVE_CAPACITY,
    .backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH,
    .persist_key = "device_remove",
};

/*
 * Локация/пояс системного устройства. FLASH: пользователь может задать место и пояс
 * вручную (web), они должны пережить перезагрузку; system обновляет запись, только
 * пока соответствующий флаг = авто (docs/services/SYSTEM.md).
 */
static const domain_entity_desc_t location_desc = {
    .type = (domain_entity_t)HA_ENTITY_LOCATION,
    .key_size = sizeof(ha_device_uid_t),
    .payload_size = sizeof(ha_location_record_t),
    .capacity = APP_LOCATION_CAPACITY,
    .backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH,
    .persist_key = "location",
};

/*
 * Экраны Display: group (экран) и group_item (виджет). Создаёт/меняет Web, читает
 * Display как клиент Domain (docs/clients/DISPLAY.md). FLASH: раскладка экранов
 * пользовательская, значит должна пережить перезагрузку.
 */
static const domain_entity_desc_t group_desc = {
    .type = (domain_entity_t)HA_ENTITY_GROUP,
    .key_size = sizeof(ha_group_key_t),
    .payload_size = sizeof(ha_group_record_t),
    .capacity = APP_GROUP_CAPACITY,
    .backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH,
    .persist_key = "group",
};

static const domain_entity_desc_t group_item_desc = {
    .type = (domain_entity_t)HA_ENTITY_GROUP_ITEM,
    .key_size = sizeof(ha_group_item_key_t),
    .payload_size = sizeof(ha_group_item_record_t),
    .capacity = APP_GROUP_ITEM_CAPACITY,
    .backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH,
    .persist_key = "group_item_v2", /* ключ теперь entity/property: старый ABI не читаем */
};

/*
 * Погода: одна сущность на синтетический uid. RAM — приходит из сети и обновляется
 * сервисом, персистентность не нужна (как location).
 */
static const domain_entity_desc_t weather_desc = {
    .type = (domain_entity_t)HA_ENTITY_WEATHER,
    .key_size = sizeof(ha_device_uid_t),
    .payload_size = sizeof(ha_weather_record_t),
    .capacity = APP_WEATHER_CAPACITY,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

/*
 * Wi-Fi provisioning (docs/services/WEB.md): скан и статус — RAM (сервис их
 * обновляет/чистит), известные точки — FLASH (автоподключение при старте).
 */
static const domain_entity_desc_t wifi_scan_desc = {
    .type = (domain_entity_t)HA_ENTITY_WIFI_SCAN,
    .key_size = sizeof(ha_device_uid_t),
    .payload_size = sizeof(ha_wifi_scan_record_t),
    .capacity = APP_WIFI_SCAN_CAPACITY,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

static const domain_entity_desc_t wifi_known_desc = {
    .type = (domain_entity_t)HA_ENTITY_WIFI_KNOWN,
    .key_size = sizeof(ha_wifi_known_key_t),
    .payload_size = sizeof(ha_wifi_known_record_t),
    .capacity = APP_WIFI_KNOWN_CAPACITY,
    .backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH,
    .persist_key = "wifi_known",
};

static const domain_entity_desc_t wifi_status_desc = {
    .type = (domain_entity_t)HA_ENTITY_WIFI_STATUS,
    .key_size = sizeof(ha_device_uid_t),
    .payload_size = sizeof(ha_wifi_status_record_t),
    .capacity = APP_WIFI_STATUS_CAPACITY,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

/* Настройки: одна запись, FLASH — должны переживать перезагрузку. */
static const domain_entity_desc_t settings_desc = {
    .type = (domain_entity_t)HA_ENTITY_SETTINGS,
    .key_size = sizeof(ha_settings_key_t),
    .payload_size = sizeof(ha_settings_record_t),
    .capacity = APP_SETTINGS_CAPACITY,
    .backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH,
    .persist_key = "settings",
};

/* Логическая сущность (Шаг A): identity детерминирована в adapter'е, record минимален (RAM). */
static const domain_entity_desc_t entity_desc = {
    .type = (domain_entity_t)HA_ENTITY_ENTITY,
    .key_size = sizeof(ha_entity_key_t),
    .payload_size = sizeof(ha_entity_record_t),
    .capacity = APP_ENTITY_CAPACITY,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

/*
 * Canonical property state (Шаг A, A1.1): семантическая истина (entity_id, property_id)
 * → ha_value_t. RAM — состояние приносят репорты заново, как и raw. Соседствует с raw
 * (state_desc) до переключения truth (A1.4): пока raw — источник, canonical наполняется
 * теневым образом (A1.2) для parity-проверки (A1.3).
 */
static const domain_entity_desc_t property_state_desc = {
    .type = (domain_entity_t)HA_ENTITY_PROPERTY_STATE,
    .key_size = sizeof(ha_state_key_t),
    .payload_size = sizeof(ha_state_record_t),
    .capacity = APP_PROPERTY_STATE_CAPACITY,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

/* Capabilities Entity (A6.1b): что сущность умеет; пишет adapter при интервью, RAM. */
static const domain_entity_desc_t capabilities_desc = {
    .type = (domain_entity_t)HA_ENTITY_CAPABILITIES,
    .key_size = sizeof(ha_entity_key_t),
    .payload_size = sizeof(ha_entity_capabilities_record_t),
    .capacity = APP_CAPABILITIES_CAPACITY,
    .backing = DOMAIN_BACKING_RAM,
    .persist_key = NULL,
};

/*
 * Правила живут как сущности: создаёт их UI/Web, читает Automation. Запись — ha_rule_t
 * (entity-адресованная, Шаг A A3.4): persistent ABI старых physical-правил несовместим.
 */
static const domain_entity_desc_t automation_desc = {
    .type = (domain_entity_t)HA_ENTITY_AUTOMATION,
    .key_size = sizeof(ha_automation_key_t),
    .payload_size = sizeof(ha_rule_t),
    .capacity = APP_AUTOMATION_CAPACITY,
    .backing = DOMAIN_BACKING_RAM | DOMAIN_BACKING_FLASH,
    .persist_key = "automation_v2", /* новая раскладка: старый ключ не читаем */
};

/*
 * Rename логической Entity (HA_CMD_ENTITY_RENAME): меняет Entity.name напрямую. Имя
 * принадлежит Entity, adapter в переименовании не участвует.
 */
static sys_error_t entity_rename_execute(domain_command_t type, const void *args, size_t args_size,
                                         void *ctx)
{
    (void)type;
    (void)ctx;
    if (args == NULL || args_size != sizeof(ha_entity_rename_t)) {
        return sys_error_make(SYS_LAYER_DOMAIN, SYS_CODE_INVALID_ARG);
    }
    const ha_entity_rename_t *request = (const ha_entity_rename_t *)args;
    const ha_entity_key_t key = {.id = request->entity};
    ha_entity_record_t record = {0};
    const sys_error_t found =
        domain_entity_get(&s_domain, (domain_entity_t)HA_ENTITY_ENTITY, &key, &record);
    if (sys_failed(found)) {
        return found;
    }

    size_t length = strnlen(request->name, sizeof(request->name));
    if (length >= sizeof(record.name)) {
        length = sizeof(record.name) - 1;
    }
    memcpy(record.name, request->name, length);
    record.name[length] = '\0';

    domain_fact_meta_t meta = {0};
    meta.source = (uint8_t)DOMAIN_SOURCE_UI;
    bool changed = false;
    return domain_entity_put(&s_domain, (domain_entity_t)HA_ENTITY_ENTITY, &key, &record, &meta,
                             &changed);
}

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
    if (!start_step(domain_register_entity(&s_domain, &automation_desc), "register automation")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &device_remove_desc), "register device-remove")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &location_desc), "register location")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &group_desc), "register group")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &group_item_desc), "register group-item")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &weather_desc), "register weather")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &wifi_scan_desc), "register wifi-scan")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &wifi_known_desc), "register wifi-known")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &wifi_status_desc), "register wifi-status")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &settings_desc), "register settings")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &entity_desc), "register entity")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &property_state_desc),
                    "register property-state")) {
        return;
    }
    if (!start_step(domain_register_entity(&s_domain, &capabilities_desc),
                    "register capabilities")) {
        return;
    }
    if (!start_step(domain_register_command(&s_domain, HA_CMD_ENTITY_RENAME,
                                            entity_rename_execute, NULL),
                    "register entity-rename")) {
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
    if (!start_step(automation_start(&s_domain), "automation start")) {
        return;
    }

    /* Wi-Fi — свой сервис: владеет радио на C3, автоподключение по известным. */
    if (!start_step(wifi_start(&s_domain), "wifi start")) {
        return;
    }

    /* Web поднимается в своей задаче; сеть ждёт от сервиса wifi. */
    if (!start_step(web_start(&s_domain), "web start")) {
        return;
    }

    /* Системное устройство (время/погода): стартует своим темпом и ждёт сеть. */
    if (!start_step(system_start(&s_domain), "system start")) {
        return;
    }

    /* Порт Display: панель ST7701 (DSI) + тач GT911, запускает UI под LVGL. */
    display_p4_start(&s_domain);

    ESP_LOGI(TAG, "bootstrap done");
}
