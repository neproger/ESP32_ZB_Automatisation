cmake_minimum_required(VERSION 3.16)

# Компоненты лежат рядом с проектом, а не в components/: у них один потребитель
# (прецедент — docs/RECORD_MODEL.md:29-32). Перечислены явно, потому что корень сам
# является проектом, и сканировать его как каталог компонентов нельзя.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/domain"
    "${CMAKE_CURRENT_LIST_DIR}/ha_model"
    "${CMAKE_CURRENT_LIST_DIR}/mstore"
    "${CMAKE_CURRENT_LIST_DIR}/sys"
    "${CMAKE_CURRENT_LIST_DIR}/zigbee"
    "${CMAKE_CURRENT_LIST_DIR}/automation")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ha_p4)
