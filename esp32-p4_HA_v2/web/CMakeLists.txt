# Собранный web-ui (vite → web/ui). Если его нет — прошивка отдаёт заглушку.
set(WEB_UI_FILES "")
foreach(f "index.html" "app.js" "app.css")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/ui/${f}")
        list(APPEND WEB_UI_FILES "ui/${f}")
    endif()
endforeach()

idf_component_register(
    SRCS
        "src/web.c"
        "src/web_proto.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        domain
        sys
        ha_model
    PRIV_REQUIRES
        esp_hosted
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        nvs_flash
    EMBED_FILES
        ${WEB_UI_FILES}
)

if(WEB_UI_FILES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WEB_UI_EMBEDDED=1)
endif()
