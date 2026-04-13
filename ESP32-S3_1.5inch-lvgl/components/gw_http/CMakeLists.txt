idf_component_register(
    SRCS
        "src/gw_http.c"
        "src/gw_ws.c"
        "src/gw_rest.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_http_server
        esp_timer
        json
        log
        spiffs
        gw_core
        gw_model
        gw_proto
        gw_zigbee
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-unused-parameter" "-Wno-maybe-uninitialized")