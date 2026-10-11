#pragma once

/*
 * Adapter-owned canonical ids адаптера Zigbee. Generic core (ha_model/domain/sys) не знает
 * о Zigbee, поэтому значения source/issuer/domain-source/error-layer объявляет сам адаптер.
 * Числа стабильны: source/issuer входят в derivation identity.
 */

#include "domain/domain_types.h"        /* domain_source_t */
#include "ha_model/ha_entity.h"         /* ha_entity_issuer_t */
#include "ha_model/ha_logical_device.h" /* ha_source_id_t */
#include "sys/sys_error.h"              /* sys_layer_t */

#define ZIGBEE_SOURCE_ID ((ha_source_id_t)1)         /* provenance Device/Entity */
#define ZIGBEE_ENTITY_ISSUER ((ha_entity_issuer_t)1) /* identity issuer */
#define ZIGBEE_DOMAIN_SOURCE ((domain_source_t)1)    /* Journal source */
#define ZIGBEE_SYS_LAYER ((sys_layer_t)3)            /* error layer */
