cmake_minimum_required(VERSION 3.16)

# Компоненты лежат рядом с проектом, а не в components/: у них один потребитель
# (прецедент — docs/RECORD_MODEL.md:29-32). Перечислены явно, потому что корень сам
# является проектом, и сканировать его как каталог компонентов нельзя.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/domain"
    "${CMAKE_CURRENT_LIST_DIR}/ha_model"
    "${CMAKE_CURRENT_LIST_DIR}/device"
    "${CMAKE_CURRENT_LIST_DIR}/mstore"
    "${CMAKE_CURRENT_LIST_DIR}/sys"
    "${CMAKE_CURRENT_LIST_DIR}/zigbee"
    "${CMAKE_CURRENT_LIST_DIR}/automation"
    "${CMAKE_CURRENT_LIST_DIR}/web"
    "${CMAKE_CURRENT_LIST_DIR}/system"
    "${CMAKE_CURRENT_LIST_DIR}/wifi"
    "${CMAKE_CURRENT_LIST_DIR}/display"
    "${CMAKE_CURRENT_LIST_DIR}/display_p4"
    # ESP-Claw: только framework-компоненты (claw_core/claw_cap и link-deps); shell
    # (app_claw/app_config/board/FATFS) не подключаем. third_party не в VCS.
    "${CMAKE_CURRENT_LIST_DIR}/claw_ha"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-claw/components/claw_modules"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-claw/components/common/http_reuse")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ha_p4)
