#pragma once

/*
 * Типы сущностей Entity Store (docs/RECORD_MODEL.md §1, §11) и generic-формы, которые
 * не относятся к конкретному транспорту.
 *
 * Формы, а не логика: только структуры и числа, никаких зависимостей. Заголовок
 * линкуют сервисы и bootstrap; Domain его не включает и этих имён не знает.
 *
 * Физических Zigbee-форм (device/state/endpoint, ZCL-ключи) здесь нет: они transport
 * vocabulary и живут в мосте внутри zigbee (zigbee/zigbee_forms.h).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Тип сущности: стабильный canonical ABI. Числа фиксированы и НИКОГДА не переиспользуются
 * (удалённое значение остаётся «дыркой»). Служит и runtime-дискриминатором Domain, и
 * wire-типом web v3 (`type = ha_entity_t`, docs/services/WEB_PROTOCOL.md §3).
 */
typedef enum {
    /*
     * 1..3 и 5 (raw Zigbee device/state/endpoint и device-remove) удалены в Шаге C:
     * transport-private хранилище адаптера и Zigbee-private deferred-leave.
     */
    HA_ENTITY_AUTOMATION = 4,
    /* Локация системного устройства (город/координаты): строка в state (число+тип) не
     * влезает, поэтому это отдельный тип сущности с текстовым полем. */
    HA_ENTITY_LOCATION = 6,
    /* Экраны Display (docs/clients/DISPLAY.md): group — экран, group_item — виджет.
     * Формы — ha_model/ha_groups.h. */
    HA_ENTITY_GROUP = 7,
    HA_ENTITY_GROUP_ITEM = 8,
    /* Погода системного сервиса (docs/services/SYSTEM.md). Форма — ha_model/ha_weather.h. */
    HA_ENTITY_WEATHER = 9,
    /* Wi-Fi provisioning (docs/services/WEB.md). Формы — ha_model/ha_wifi.h. */
    HA_ENTITY_WIFI_SCAN = 10,
    HA_ENTITY_WIFI_KNOWN = 11,
    HA_ENTITY_WIFI_STATUS = 12,
    /* Настройки приложения (docs/services/SETTINGS.md). Форма — ha_model/ha_settings.h. */
    HA_ENTITY_SETTINGS = 13,
    /* Логическая сущность (Шаг A): transport-agnostic identity. Форма — ha_model/ha_entity.h. */
    HA_ENTITY_ENTITY = 14,
    /* Canonical property state (Шаг A): единственный production state — семантическая
     * истина (entity_id, property_id). Форма — ha_model/ha_state.h. */
    HA_ENTITY_PROPERTY_STATE = 15,
    /* Canonical capabilities (Шаг A): что Entity умеет. Форма — ha_model/ha_capabilities.h. */
    HA_ENTITY_CAPABILITIES = 16,
    /* Logical Device (Шаг B): логический узел/вещь, которой принадлежат Entity. Форма —
     * ha_model/ha_logical_device.h. Transport-agnostic, Domain не знает EUI64. */
    HA_ENTITY_LOGICAL_DEVICE = 17,
} ha_entity_t;

/* Локация (ключ — системный device id): текст города + координаты + tz-offset. */
#define HA_LOCATION_NAME_MAX 48

/*
 * Флаги локации: выставленный бит = значение берётся автоматически (GeoIP), снятый —
 * ручное значение пользователя (тогда system его не перетирает).
 */
#define HA_LOCATION_FLAG_TZ_AUTO 0x01u  /* пояс: авто (GeoIP) / вручную tz_offset_min */
#define HA_LOCATION_FLAG_POS_AUTO 0x02u /* место: авто (GeoIP) / вручную name+lat+lon */

typedef struct {
    float latitude;
    float longitude;
    int16_t tz_offset_min;
    uint8_t flags; /* HA_LOCATION_FLAG_* */
    uint8_t reserved;
    char name[HA_LOCATION_NAME_MAX];
} ha_location_record_t;

#ifdef __cplusplus
static_assert(sizeof(ha_location_record_t) == 60, "ha_location_record_t: неожиданный размер");
static_assert(offsetof(ha_location_record_t, name) == 12, "ha_location_record_t: layout");
#else
_Static_assert(sizeof(ha_location_record_t) == 60, "ha_location_record_t: неожиданный размер");
_Static_assert(offsetof(ha_location_record_t, name) == 12, "ha_location_record_t: layout");
#endif

#ifdef __cplusplus
}
#endif
