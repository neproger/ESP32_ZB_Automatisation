# Generic Device service (Шаг B): нормализованный ha_device_desc_t → canonical Domain
# identity (Logical Device + ownership). Не знает транспорта; Domain — хранилище.
# Граница: UI/бизнес -> Device service -> Domain.
idf_component_register(
    SRCS
        "src/device_service.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        domain
        ha_model
        sys
)
