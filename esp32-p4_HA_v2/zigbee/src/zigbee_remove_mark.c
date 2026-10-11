#include "zigbee/zigbee_remove_mark.h"

#include "zigbee/zigbee_adapter_ids.h"

#include <string.h>

#include "nvs.h"

#define ZB_REMOVE_NS "zb_rm"
#define ZB_REMOVE_KEY "list"
#define ZB_REMOVE_MARK_MAX 16

typedef struct {
    uint32_t count;
    uint64_t uids[ZB_REMOVE_MARK_MAX];
} zb_remove_blob_t;

static zb_remove_blob_t s_marks;
static bool s_loaded;

static void load_once(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    memset(&s_marks, 0, sizeof(s_marks));

    nvs_handle_t handle = 0;
    if (nvs_open(ZB_REMOVE_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_marks);
    if (nvs_get_blob(handle, ZB_REMOVE_KEY, &s_marks, &len) != ESP_OK ||
        s_marks.count > ZB_REMOVE_MARK_MAX) {
        memset(&s_marks, 0, sizeof(s_marks));
    }
    nvs_close(handle);
}

static sys_error_t save(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ZB_REMOVE_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, ZB_REMOVE_KEY, &s_marks, sizeof(s_marks));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return err == ESP_OK ? SYS_OK : sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_IO);
}

sys_error_t zigbee_remove_mark_add(ha_device_uid_t uid)
{
    load_once();
    for (uint32_t i = 0; i < s_marks.count; i++) {
        if (s_marks.uids[i] == uid) {
            return SYS_OK;
        }
    }
    if (s_marks.count >= ZB_REMOVE_MARK_MAX) {
        return sys_error_make(ZIGBEE_SYS_LAYER, SYS_CODE_NO_MEM);
    }
    s_marks.uids[s_marks.count++] = uid;
    return save();
}

bool zigbee_remove_mark_has(ha_device_uid_t uid)
{
    load_once();
    for (uint32_t i = 0; i < s_marks.count; i++) {
        if (s_marks.uids[i] == uid) {
            return true;
        }
    }
    return false;
}

void zigbee_remove_mark_clear(ha_device_uid_t uid)
{
    load_once();
    for (uint32_t i = 0; i < s_marks.count; i++) {
        if (s_marks.uids[i] == uid) {
            s_marks.uids[i] = s_marks.uids[--s_marks.count];
            (void)save();
            return;
        }
    }
}
