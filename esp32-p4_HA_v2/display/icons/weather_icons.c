#include "weather_icons.h"

#include "ha_model/ha_weather.h"

const lv_image_dsc_t *weather_icon_for(uint8_t condition)
{
    switch (condition) {
    case HA_WEATHER_CLEAR:
        return &wicon_sunny;
    case HA_WEATHER_CLEAR_NIGHT:
        return &wicon_clear_night;
    case HA_WEATHER_PARTLYCLOUDY:
        return &wicon_partlycloudy;
    case HA_WEATHER_CLOUDY:
        return &wicon_cloudy;
    case HA_WEATHER_OVERCAST:
        return &wicon_overcast;
    case HA_WEATHER_FOG:
        return &wicon_fog;
    case HA_WEATHER_RAINY:
        return &wicon_rainy;
    case HA_WEATHER_POURING:
        return &wicon_pouring;
    case HA_WEATHER_SNOWY:
        return &wicon_snowy;
    case HA_WEATHER_SNOWY_RAINY:
        return &wicon_snowy_rainy;
    case HA_WEATHER_HAIL:
        return &wicon_hail;
    case HA_WEATHER_LIGHTNING:
        return &wicon_lightning;
    case HA_WEATHER_LIGHTNING_RAINY:
        return &wicon_lightning_rainy;
    case HA_WEATHER_WINDY:
        return &wicon_windy;
    case HA_WEATHER_WINDY_VARIANT:
        return &wicon_windy_variant;
    default:
        return &wicon_exceptional;
    }
}
