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
    ../src/storage/mstore_storage_flash.c
    ../src/storage/mstore_storage_ram_flash.c
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

# NOR simulator — host-only support, без core.
add_executable(test_flash_sim test_flash_sim.c support/nor_sim.c)
target_include_directories(test_flash_sim PRIVATE support)
if(MSVC)
    target_compile_options(test_flash_sim PRIVATE /W4)
else()
    target_compile_options(test_flash_sim PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_flash_sim COMMAND test_flash_sim)

# FLASH backend поверх NOR simulator.
add_executable(test_flash test_flash.c support/nor_sim.c support/nor_sim_device.c
    ${MSTORE_CORE} ${MSTORE_HOST_PORT})
target_include_directories(test_flash PRIVATE ../include ../src support)
if(MSVC)
    target_compile_options(test_flash PRIVATE /W4)
else()
    target_compile_options(test_flash PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_flash COMMAND test_flash)

# Общий behavioral suite (RAM + FLASH) и FLASH durability.
foreach(flash_test IN ITEMS test_behavior test_flash_durability test_flash_model test_composite)
    add_executable(${flash_test} ${flash_test}.c
        support/nor_sim.c support/nor_sim_device.c
        ${MSTORE_CORE} ${MSTORE_HOST_PORT})
    target_include_directories(${flash_test} PRIVATE ../include ../src support)
    if(MSVC)
        target_compile_options(${flash_test} PRIVATE /W4)
    else()
        target_compile_options(${flash_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${flash_test} COMMAND ${flash_test})
endforeach()
