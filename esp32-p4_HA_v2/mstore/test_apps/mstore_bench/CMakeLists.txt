cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Счётчики FLASH backend включаются только здесь: это benchmark build, обычная
# сборка mstore их не содержит.
idf_build_set_property(COMPILE_DEFINITIONS "MSTORE_BENCH_COUNTERS=1" APPEND)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

project(mstore_bench)
