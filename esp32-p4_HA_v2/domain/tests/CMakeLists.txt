cmake_minimum_required(VERSION 3.16)
project(domain_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(DOMAIN_CORE ../src/domain_registry.c ../src/domain_entity.c ../src/domain_journal.c
    ../src/domain_dispatch.c)

set(MSTORE_CORE
    ../../mstore/src/mstore_table.c
    ../../mstore/src/mstore_runtime.c
    ../../mstore/src/mstore_invariants.c
    ../../mstore/src/mstore_ring.c
    ../../mstore/src/storage/mstore_storage.c
    ../../mstore/src/storage/mstore_bytes.c
    ../../mstore/src/storage/mstore_region.c
    ../../mstore/src/storage/mstore_storage_ram.c
    ../../mstore/src/storage/mstore_storage_flash.c
    ../../mstore/src/storage/mstore_storage_ram_flash.c
)
set(MSTORE_HOST_PORT ../../mstore/port/host/mstore_platform_host.c)
set(DOMAIN_HOST_PORT ../port/host/domain_platform_host.c)

function(domain_add_test name)
    add_executable(${name} ${name}.c ${DOMAIN_CORE} ${DOMAIN_HOST_PORT} ${MSTORE_CORE}
        ${MSTORE_HOST_PORT})
    target_include_directories(${name} PRIVATE ../include ../src ../../mstore/include ../../mstore/src)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

domain_add_test(test_registry)
domain_add_test(test_entity)
domain_add_test(test_concurrency)
domain_add_test(test_journal)
domain_add_test(test_facts)
domain_add_test(test_dispatch)

# Persistence-сценарий: Domain поверх FLASH-backed mstore на host NOR simulator.
# nor_sim — инфраструктура окружения из тестов mstore, не часть Domain.
set(NOR_SIM_SUPPORT
    ../../mstore/tests/support/nor_sim.c
    ../../mstore/tests/support/nor_sim_device.c
)

add_executable(test_flash_reopen test_flash_reopen.c ${NOR_SIM_SUPPORT} ${DOMAIN_CORE}
    ${DOMAIN_HOST_PORT} ${MSTORE_CORE} ${MSTORE_HOST_PORT})
target_include_directories(test_flash_reopen PRIVATE ../include ../src ../../mstore/include
    ../../mstore/src ../../mstore/tests/support)
if(MSVC)
    target_compile_options(test_flash_reopen PRIVATE /W4)
else()
    target_compile_options(test_flash_reopen PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_flash_reopen COMMAND test_flash_reopen)
