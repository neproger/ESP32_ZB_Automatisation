# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wno-format")
project(s3_automation_hub)

# Pack Web UI into SPIFFS partition `www` if build artifacts exist.
if(EXISTS "${CMAKE_SOURCE_DIR}/web/dist/index.html")
    spiffs_create_partition_image(www "${CMAKE_SOURCE_DIR}/web/dist" FLASH_IN_PROJECT)
endif()
