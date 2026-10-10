# Собранный web-ui (vite + gzip → web/ui). index.html — как есть, скрипты/стили — *.gz
# (отдаются с Content-Encoding: gzip). Если файлов нет — прошивка отдаёт заглушку.
set(WEB_UI_FILES "")
foreach(f "index.html" "app.js.gz" "app.css.gz")
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
        semantics
    PRIV_REQUIRES
        esp_http_server
        nvs_flash
    EMBED_FILES
        ${WEB_UI_FILES}
)

if(WEB_UI_FILES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WEB_UI_EMBEDDED=1)
endif()
