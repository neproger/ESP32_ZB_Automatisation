#include "ha_model/ha_logical_device.h"

#include <string.h>

#include "ha_model/ha_entity.h" /* ha_entity_id_derive, HA_ENTITY_ID_DERIVE_SEED_MAX */

ha_device_id_t ha_device_id_derive(ha_source_id_t source, const char *native_id)
{
    if (native_id == NULL) {
        return HA_DEVICE_ID_NONE;
    }
    const size_t length = strlen(native_id);
    if (length == 0 || length > HA_ENTITY_ID_DERIVE_SEED_MAX) {
        return HA_DEVICE_ID_NONE;
    }
    return (ha_device_id_t)ha_entity_id_derive((uint32_t)source, native_id, length);
}
