idf_component_register(SRCS "zigbee_poc.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp-zigbee-lib nvs_flash)
