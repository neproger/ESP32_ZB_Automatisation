cmake_minimum_required(VERSION 3.16)

# Step C1 spike: минимальный P4-хост claw_core + claw_cap + dummy capability.
# Только framework-компоненты esp-claw; shell (app_claw/app_config/board/FATFS) не тянем.
set(COMPONENTS main)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-claw/components/claw_modules"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/esp-claw/components/common/http_reuse")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_claw_spike)
