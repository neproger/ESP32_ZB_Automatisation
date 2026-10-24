#include "zigbee/zigbee_device_store.h"

#include "zigbee/zigbee_adapter_ids.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "zb_store";
#endif

#define ZB_DEVICE_STORE_MAX 32
#define ZB_ENDPOINT_STORE_MAX 64

typedef struct {
    bool used;
    ha_device_uid_t uid;
    ha_device_record_t record;
} zb_device_slot_t;

typedef struct {
    bool used;
    ha_device_uid_t uid;
    uint8_t endpoint;
    ha_endpoint_record_t record;
} zb_endpoint_slot_t;

static zb_device_slot_t s_devices[ZB_DEVICE_STORE_MAX];
static zb_endpoint_slot_t s_endpoints[ZB_ENDPOINT_STORE_MAX];

static sys_error_t zigbee_fail(sys_code_t code)
{
    return sys_error_make(ZIGBEE_SYS_LAYER, code);
}

static zb_device_slot_t *device_find(ha_device_uid_t uid)
{
    for (size_t i = 0; i < ZB_DEVICE_STORE_MAX; i++) {
        if (s_devices[i].used && s_devices[i].uid == uid) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static zb_device_slot_t *device_alloc(ha_device_uid_t uid)
{
    for (size_t i = 0; i < ZB_DEVICE_STORE_MAX; i++) {
        if (!s_devices[i].used) {
            s_devices[i].used = true;
            s_devices[i].uid = uid;
            memset(&s_devices[i].record, 0, sizeof(s_devices[i].record));
            return &s_devices[i];
        }
    }
    return NULL;
}

sys_error_t zigbee_device_set_identity(ha_device_uid_t uid, const char *manufacturer,
                                       const char *model, bool *out_changed)
{
    if (manufacturer == NULL || model == NULL || out_changed == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    *out_changed = false;

    zb_device_slot_t *slot = device_find(uid);
    if (slot == NULL) {
        slot = device_alloc(uid);
        if (slot == NULL) {
            return zigbee_fail(SYS_CODE_NO_MEM);
        }
        *out_changed = true;
    }

    char trimmed[HA_DEVICE_NAME_MAX] = {0};
    size_t length = strlen(manufacturer);
    if (length >= sizeof(trimmed)) {
        length = sizeof(trimmed) - 1;
    }
    memcpy(trimmed, manufacturer, length);
    if (strncmp(slot->record.manufacturer, trimmed, sizeof(slot->record.manufacturer)) != 0) {
        memcpy(slot->record.manufacturer, trimmed, sizeof(slot->record.manufacturer));
        *out_changed = true;
    }

    memset(trimmed, 0, sizeof(trimmed));
    length = strlen(model);
    if (length >= HA_DEVICE_MODEL_MAX) {
        length = HA_DEVICE_MODEL_MAX - 1;
    }
    memcpy(trimmed, model, length);
    if (strncmp(slot->record.model, trimmed, sizeof(slot->record.model)) != 0) {
        memcpy(slot->record.model, trimmed, sizeof(slot->record.model));
        *out_changed = true;
    }
    return SYS_OK;
}

bool zigbee_device_get(ha_device_uid_t uid, ha_device_record_t *out)
{
    if (out == NULL) {
        return false;
    }
    const zb_device_slot_t *slot = device_find(uid);
    if (slot == NULL) {
        return false;
    }
    *out = slot->record;
    return true;
}

bool zigbee_device_has(ha_device_uid_t uid)
{
    return device_find(uid) != NULL;
}

static zb_endpoint_slot_t *endpoint_find(ha_device_uid_t uid, uint8_t endpoint)
{
    for (size_t i = 0; i < ZB_ENDPOINT_STORE_MAX; i++) {
        if (s_endpoints[i].used && s_endpoints[i].uid == uid &&
            s_endpoints[i].endpoint == endpoint) {
            return &s_endpoints[i];
        }
    }
    return NULL;
}

sys_error_t zigbee_topology_apply(const zigbee_endpoint_desc_t *desc, bool *out_changed)
{
    if (desc == NULL || out_changed == NULL || desc->clusters == NULL) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }
    if (desc->cluster_count == 0 || desc->cluster_count > HA_ENDPOINT_CLUSTERS_MAX) {
        return zigbee_fail(SYS_CODE_INVALID_ARG);
    }

    *out_changed = false;

    if (device_find(desc->device_uid) == NULL) {
        if (device_alloc(desc->device_uid) == NULL) {
            return zigbee_fail(SYS_CODE_NO_MEM);
        }
        *out_changed = true;
    }

    ha_endpoint_record_t record = {0};
    record.profile_id = desc->profile_id;
    record.device_id = desc->device_id;
    record.cluster_count = desc->cluster_count;
    memcpy(record.clusters, desc->clusters, desc->cluster_count * sizeof(ha_cluster_entry_t));

    zb_endpoint_slot_t *slot = endpoint_find(desc->device_uid, desc->endpoint);
    if (slot == NULL) {
        for (size_t i = 0; i < ZB_ENDPOINT_STORE_MAX; i++) {
            if (!s_endpoints[i].used) {
                slot = &s_endpoints[i];
                slot->used = true;
                slot->uid = desc->device_uid;
                slot->endpoint = desc->endpoint;
                break;
            }
        }
        if (slot == NULL) {
            return zigbee_fail(SYS_CODE_NO_MEM);
        }
        slot->record = record;
        *out_changed = true;
        return SYS_OK;
    }

    if (memcmp(&slot->record, &record, sizeof(record)) != 0) {
        slot->record = record;
        *out_changed = true;
    }
    return SYS_OK;
}

bool zigbee_topology_get(ha_device_uid_t uid, uint8_t endpoint, ha_endpoint_record_t *out)
{
    if (out == NULL) {
        return false;
    }
    const zb_endpoint_slot_t *slot = endpoint_find(uid, endpoint);
    if (slot == NULL) {
        return false;
    }
    *out = slot->record;
    return true;
}

sys_error_t zigbee_device_remove(ha_device_uid_t uid)
{
    zb_device_slot_t *slot = device_find(uid);
    if (slot != NULL) {
        slot->used = false;
    }
    for (size_t i = 0; i < ZB_ENDPOINT_STORE_MAX; i++) {
        if (s_endpoints[i].used && s_endpoints[i].uid == uid) {
            s_endpoints[i].used = false;
        }
    }
    return SYS_OK;
}

/* --- обход (для restore-прохода) ---------------------------------------- */

void zigbee_device_store_foreach(zigbee_device_visit_fn fn, void *ctx)
{
    if (fn == NULL) {
        return;
    }
    for (size_t i = 0; i < ZB_DEVICE_STORE_MAX; i++) {
        if (s_devices[i].used && !fn(s_devices[i].uid, &s_devices[i].record, ctx)) {
            return;
        }
    }
}

void zigbee_endpoints_foreach(ha_device_uid_t uid, zigbee_endpoint_visit_fn fn, void *ctx)
{
    if (fn == NULL) {
        return;
    }
    for (size_t i = 0; i < ZB_ENDPOINT_STORE_MAX; i++) {
        if (s_endpoints[i].used && s_endpoints[i].uid == uid &&
            !fn(s_endpoints[i].endpoint, &s_endpoints[i].record, ctx)) {
            return;
        }
    }
}

uint8_t zigbee_endpoint_count(ha_device_uid_t uid)
{
    uint8_t n = 0;
    for (size_t i = 0; i < ZB_ENDPOINT_STORE_MAX; i++) {
        if (s_endpoints[i].used && s_endpoints[i].uid == uid) {
            n++;
        }
    }
    return n;
}

/* Persistence — только на железе (flash-раздел); host-тесты компилируют RAM-часть. */
#if defined(ESP_PLATFORM)
/* --- persistence (flash, раздел "zb_model") ------------------------------ */

#define ZB_STORE_MAGIC0 'Z'
#define ZB_STORE_MAGIC1 'M'
#define ZB_STORE_MAGIC2 'O'
#define ZB_STORE_MAGIC3 'D'
#define ZB_STORE_VER 2u /* v2: ha_device_record_t + manufacturer; старый блоб не читаем (устройства добавляются заново) */
#define ZB_STORE_HDR 16u
#define ZB_DEV_SIZE (8u + sizeof(ha_device_record_t))
#define ZB_EP_SIZE (8u + 4u + sizeof(ha_endpoint_record_t))
#define ZB_STORE_BLOB_MAX \
    (ZB_STORE_HDR + ZB_DEVICE_STORE_MAX * ZB_DEV_SIZE + ZB_ENDPOINT_STORE_MAX * ZB_EP_SIZE)

static uint8_t s_blob[ZB_STORE_BLOB_MAX];
static const esp_partition_t *s_part;

sys_error_t zigbee_device_store_save(void)
{
    if (s_part == NULL) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                          "zb_model");
    }
    if (s_part == NULL) {
        return zigbee_fail(SYS_CODE_NOT_FOUND);
    }

    memset(s_blob, 0, sizeof(s_blob));
    size_t off = ZB_STORE_HDR;
    uint16_t dev_count = 0;
    uint16_t ep_count = 0;
    for (size_t i = 0; i < ZB_DEVICE_STORE_MAX; i++) {
        if (!s_devices[i].used) {
            continue;
        }
        memcpy(s_blob + off, &s_devices[i].uid, 8);
        memcpy(s_blob + off + 8, &s_devices[i].record, sizeof(ha_device_record_t));
        off += ZB_DEV_SIZE;
        dev_count++;
    }
    for (size_t i = 0; i < ZB_ENDPOINT_STORE_MAX; i++) {
        if (!s_endpoints[i].used) {
            continue;
        }
        memcpy(s_blob + off, &s_endpoints[i].uid, 8);
        s_blob[off + 8] = s_endpoints[i].endpoint;
        memcpy(s_blob + off + 12, &s_endpoints[i].record, sizeof(ha_endpoint_record_t));
        off += ZB_EP_SIZE;
        ep_count++;
    }

    s_blob[0] = ZB_STORE_MAGIC0;
    s_blob[1] = ZB_STORE_MAGIC1;
    s_blob[2] = ZB_STORE_MAGIC2;
    s_blob[3] = ZB_STORE_MAGIC3;
    s_blob[4] = (uint8_t)ZB_STORE_VER;
    s_blob[5] = 0;
    memcpy(s_blob + 6, &dev_count, 2);
    memcpy(s_blob + 8, &ep_count, 2);
    const uint32_t crc = esp_rom_crc32_le(0, s_blob + ZB_STORE_HDR, (uint32_t)(off - ZB_STORE_HDR));
    memcpy(s_blob + 12, &crc, 4);

    esp_err_t err = esp_partition_erase_range(s_part, 0, s_part->size);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, 0, s_blob, off);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return zigbee_fail(SYS_CODE_IO);
    }
    ESP_LOGI(TAG, "saved: devices=%u endpoints=%u (%u bytes)", (unsigned)dev_count,
             (unsigned)ep_count, (unsigned)off);
    return SYS_OK;
}

sys_error_t zigbee_device_store_load(void)
{
    if (s_part == NULL) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                          "zb_model");
    }
    if (s_part == NULL) {
        ESP_LOGW(TAG, "partition 'zb_model' not found; store not persisted");
        return SYS_OK; /* пустой store — устройства переоткроются интервью */
    }

    uint8_t hdr[ZB_STORE_HDR];
    if (esp_partition_read(s_part, 0, hdr, sizeof(hdr)) != ESP_OK) {
        return zigbee_fail(SYS_CODE_IO);
    }
    if (hdr[0] != ZB_STORE_MAGIC0 || hdr[1] != ZB_STORE_MAGIC1 || hdr[2] != ZB_STORE_MAGIC2 ||
        hdr[3] != ZB_STORE_MAGIC3 || hdr[4] != (uint8_t)ZB_STORE_VER) {
        return SYS_OK; /* нет валидного блоба */
    }
    uint16_t dev_count = 0;
    uint16_t ep_count = 0;
    uint32_t crc_stored = 0;
    memcpy(&dev_count, hdr + 6, 2);
    memcpy(&ep_count, hdr + 8, 2);
    memcpy(&crc_stored, hdr + 12, 4);
    if (dev_count > ZB_DEVICE_STORE_MAX || ep_count > ZB_ENDPOINT_STORE_MAX) {
        return SYS_OK;
    }
    const size_t len = ZB_STORE_HDR + (size_t)dev_count * ZB_DEV_SIZE +
                       (size_t)ep_count * ZB_EP_SIZE;
    if (esp_partition_read(s_part, 0, s_blob, len) != ESP_OK) {
        return zigbee_fail(SYS_CODE_IO);
    }
    const uint32_t crc = esp_rom_crc32_le(0, s_blob + ZB_STORE_HDR, (uint32_t)(len - ZB_STORE_HDR));
    if (crc != crc_stored) {
        ESP_LOGW(TAG, "store crc mismatch; ignoring");
        return SYS_OK;
    }

    memset(s_devices, 0, sizeof(s_devices));
    memset(s_endpoints, 0, sizeof(s_endpoints));

    size_t off = ZB_STORE_HDR;
    for (uint16_t i = 0; i < dev_count; i++) {
        zb_device_slot_t *slot = &s_devices[i];
        slot->used = true;
        memcpy(&slot->uid, s_blob + off, 8);
        memcpy(&slot->record, s_blob + off + 8, sizeof(ha_device_record_t));
        off += ZB_DEV_SIZE;
    }
    for (uint16_t i = 0; i < ep_count; i++) {
        zb_endpoint_slot_t *slot = &s_endpoints[i];
        slot->used = true;
        memcpy(&slot->uid, s_blob + off, 8);
        slot->endpoint = s_blob[off + 8];
        memcpy(&slot->record, s_blob + off + 12, sizeof(ha_endpoint_record_t));
        off += ZB_EP_SIZE;
    }
    ESP_LOGI(TAG, "loaded: devices=%u endpoints=%u", (unsigned)dev_count, (unsigned)ep_count);
    return SYS_OK;
}
#else
sys_error_t zigbee_device_store_save(void)
{
    return SYS_OK; /* host: flash нет */
}

sys_error_t zigbee_device_store_load(void)
{
    return SYS_OK;
}
#endif
