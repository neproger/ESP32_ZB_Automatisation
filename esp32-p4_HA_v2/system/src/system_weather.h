#pragma once

#include <stdbool.h>

#include "ha_model/ha_weather.h"

/* Open-Meteo: WMO-код + температура/влажность/ветер по координатам. false — недоступно. */
bool system_weather_fetch(double lat, double lon, ha_weather_record_t *out);
