cmake_minimum_required(VERSION 3.16)
project(mstore_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(MSTORE_CORE
    ../src/mstore_table.c
    ../src/mstore_runtime.c
    ../src/mstore_invariants.c
    ../src/mstore_ring.c
    ../src/storage/mstore_storage.c
    ../src/storage/mstore_storage_ram.c
)
set(MSTORE_HOST_PORT ../port/host/mstore_platform_host.c)

function(mstore_add_test name)
    add_executable(${name} ${name}.c ${MSTORE_CORE} ${MSTORE_HOST_PORT})
    target_include_directories(${name} PRIVATE ../include ../src)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

mstore_add_test(test_table)
mstore_add_test(test_model)
mstore_add_test(test_ring)
mstore_add_test(test_ring_model)
